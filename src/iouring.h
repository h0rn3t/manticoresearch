//
// Copyright (c) 2026, Manticore Software LTD (https://manticoresearch.com)
// All rights reserved
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

#ifndef _iouring_
#define _iouring_

// Linux io_uring async disk reader backend.
//
// This is the platform layer: it owns the io_uring ring and a dedicated reaper
// thread that turns completions (CQE) into callback invocations. It deliberately
// depends only on liburing + pthreads, so it can be unit-compiled and tested in
// isolation from the rest of the daemon. The coroutine glue (submit a read,
// suspend the fiber, resume it on completion) lives elsewhere and passes a
// callback that wakes the fiber.
//
// When HAVE_IO_URING is not defined (non-Linux, or liburing absent) this header
// still compiles, but IsIoUringAvailable() returns false and callers fall back
// to blocking sphPread.

#include <cstdint>
#include <functional>

namespace IoUring
{

/// Result delivered to a read completion callback.
/// iRes mirrors io_uring cqe.res: >=0 is bytes read, <0 is -errno.
struct ReadResult_t
{
	int m_iRes = 0;
};

/// Callback invoked from the reaper thread when a submitted read completes.
using OnComplete_fn = std::function<void ( ReadResult_t )>;

/// True if the daemon was built with io_uring support AND the running kernel
/// accepts io_uring_setup at runtime (i.e. not blocked by seccomp/old kernel).
/// Cheap after the first call (result is cached).
bool IsIoUringAvailable();

/// Start the global io_uring backend (ring + reaper thread).
/// bSQPoll requests kernel-side submission polling (IORING_SETUP_SQPOLL), which
/// avoids a submit syscall per read at the cost of a busy kernel poll thread;
/// it falls back to normal mode if the kernel refuses it.
/// Returns false and leaves the backend disabled if io_uring is unavailable;
/// callers must then use the synchronous path. Idempotent.
bool StartIoUring ( unsigned uQueueDepth = 1024, bool bSQPoll = false );

/// True if the running backend negotiated SQPOLL (for diagnostics/logging).
bool IoUringUsesSQPoll();

/// Stop the reaper thread and tear down the ring. Idempotent.
void StopIoUring();

/// Submit an async pread of iBytes at iOffset from iFD into pBuf.
/// On completion fnDone is invoked (from the reaper thread) with the result.
/// pBuf must stay valid until the callback fires.
/// Returns false if the read could not be submitted (backend down or ring full);
/// the caller should then fall back to a synchronous read. fnDone is NOT called
/// when this returns false.
bool SubmitRead ( int iFD, void * pBuf, unsigned iBytes, int64_t iOffset, OnComplete_fn fnDone );

} // namespace IoUring

#endif // _iouring_
