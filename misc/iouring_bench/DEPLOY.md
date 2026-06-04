# Deploying the io_uring build on Ubuntu 24.04

Lean .deb package (full-text search + io_uring; ODBC/CURL/PostgreSQL source
connectors are disabled — see the packaging workflow). Built for jammy/x86_64,
installs and runs on Ubuntu 22.04/24.04 (bundled deps; liburing is statically
vendored, so there is no runtime liburing dependency).

## 1. Get the package

From the `📦 io_uring package (deb)` workflow run, download the `iouring_deb`
artifact. It is a zip that contains a `artifact.tar`; unpack both:

```bash
# locally (needs gh CLI authed to the fork)
gh run download <RUN_ID> -R h0rn3t/manticoresearch -n iouring_deb -D iouring_deb
cd iouring_deb && tar xf artifact.tar      # -> build/*.deb
```

(Or download the zip from the Actions UI, unzip, then `tar xf artifact.tar`.)

Copy the runtime debs to the server:

```bash
scp build/manticore-common_*_all.deb \
    build/manticore-server-core_*_amd64.deb \
    build/manticore-server_*_amd64.deb \
    build/manticore-tools_*_amd64.deb \
    build/manticore-icudata-*.deb \
    user@server:/tmp/mdeb/
```

## 2. Install

```bash
sudo apt install /tmp/mdeb/manticore-common_*_all.deb \
                 /tmp/mdeb/manticore-icudata-*.deb \
                 /tmp/mdeb/manticore-server-core_*_amd64.deb \
                 /tmp/mdeb/manticore-server_*_amd64.deb \
                 /tmp/mdeb/manticore-tools_*_amd64.deb
```

apt resolves the inter-package and system deps. (`.ddeb` dbgsym packages are
optional — install only if you want debug symbols.)

## 3. Enable io_uring in the config

Edit `/etc/manticoresearch/manticore.conf`, in the `searchd { }` section:

```
searchd {
    # ... existing settings ...
    io_uring = 1            # async disk reads for FILE-mode doclists/hitlists (default on)
    # io_uring_sqpoll = 1   # kernel-side submit polling: ONLY on a multi-core box;
                            # it dedicates a CPU to a poll thread.
}
```

io_uring applies to `access_doclists = file` / `access_hitlists = file` (the
defaults). It auto-falls back to blocking `pread` if io_uring is unavailable, so
enabling it is always safe.

## 4. Start and verify

```bash
sudo systemctl enable --now manticore
sudo systemctl status manticore
grep -i io_uring /var/log/manticore/searchd.log
#   expect:  io_uring: enabled (async disk reads)
#   with sqpoll:  io_uring: enabled (async disk reads, SQPOLL)
```

## Prerequisites & notes

- **Kernel**: Ubuntu 24.04 ships 6.8 — full io_uring. (5.10+ minimum.)
- **Containers**: running searchd directly under systemd is fine. Inside
  Docker/k8s the default seccomp profile blocks `io_uring_setup` (searchd then
  logs "requested but unavailable" and uses pread). Allow the io_uring syscalls
  in the seccomp profile if you want it there.
- **Where it helps**: io_uring overlaps real disk reads. On a hot page cache it
  is parity with pread (overhead without benefit). The win shows on cold cache /
  index >> RAM / high concurrency on NVMe. Benchmark with your own data and
  `echo 3 | sudo tee /proc/sys/vm/drop_caches` between cold runs.
```
