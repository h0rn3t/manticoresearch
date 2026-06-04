// io_uring smoke test: submit a real pread via the ring, reap the CQE, verify bytes.
// Purpose: prove io_uring works inside the colima VM / docker container (kernel + seccomp).
#include <liburing.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

int main(void) {
    struct io_uring ring;
    int rc = io_uring_queue_init(8, &ring, 0);
    if (rc < 0) {
        fprintf(stderr, "FAIL io_uring_queue_init: %s (errno-like %d)\n", strerror(-rc), -rc);
        return 2; // distinguishes "setup blocked" (seccomp/ENOSYS/EPERM)
    }
    printf("OK  io_uring_queue_init\n");

    const char *path = "/etc/hostname";
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return 3; }

    char buf[256];
    memset(buf, 0, sizeof(buf));

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    io_uring_prep_read(sqe, fd, buf, sizeof(buf) - 1, 0);
    sqe->user_data = 0x42;

    rc = io_uring_submit(&ring);
    if (rc < 0) { fprintf(stderr, "FAIL submit: %s\n", strerror(-rc)); return 4; }
    printf("OK  io_uring_submit (%d sqe)\n", rc);

    struct io_uring_cqe *cqe;
    rc = io_uring_wait_cqe(&ring, &cqe);
    if (rc < 0) { fprintf(stderr, "FAIL wait_cqe: %s\n", strerror(-rc)); return 5; }
    if (cqe->user_data != 0x42) { fprintf(stderr, "FAIL user_data mismatch\n"); return 6; }
    if (cqe->res < 0) { fprintf(stderr, "FAIL read res: %s\n", strerror(-cqe->res)); return 7; }

    int n = cqe->res;
    io_uring_cqe_seen(&ring, cqe);
    printf("OK  async read %d bytes from %s: \"%.*s\"\n", n, path, n > 40 ? 40 : n, buf);

    io_uring_queue_exit(&ring);
    close(fd);
    printf("ALL OK: io_uring fully functional in this environment\n");
    return 0;
}
