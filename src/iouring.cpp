//
// Copyright (c) 2026, Manticore Software LTD (https://manticoresearch.com)
// All rights reserved
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

#include "iouring.h"

#if HAVE_IO_URING

#include <liburing.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <cstdint>

namespace IoUring
{

namespace
{
	// Sentinel user_data submitted on shutdown to wake the reaper out of wait_cqe.
	// Real completions carry a heap Completion_t pointer, which is never this value.
	constexpr uint64_t STOP_SENTINEL = UINT64_MAX;

	struct Completion_t
	{
		OnComplete_fn m_fnDone;
	};

	struct State_t
	{
		io_uring		m_tRing {};
		std::mutex		m_tSubmitMutex;			// SQ is single-producer: serialize submitters
		std::thread		m_tReaper;
		std::atomic<bool> m_bRunning { false };
		std::atomic<int> m_iInflight { 0 };		// reads submitted but not yet reaped (the throttle counter)
		unsigned		m_uMaxInflight = 0;		// cap on m_iInflight; set once at start, treated const while running
		bool			m_bRingInit = false;
		bool			m_bSQPoll = false;
	};

	State_t g_tState;

	void ReaperLoop()
	{
		while ( true )
		{
			io_uring_cqe * pCqe = nullptr;
			int iRc = io_uring_wait_cqe ( &g_tState.m_tRing, &pCqe );
			if ( iRc<0 )
			{
				// EINTR and friends: just retry. Any other error: bail to avoid a busy loop.
				if ( iRc==-EINTR )
					continue;
				break;
			}

			// Drain every completion available in this wakeup in one batch, then
			// advance the CQ head once - far cheaper than one wait_cqe per CQE.
			bool bStop = false;
			unsigned uHead = 0;
			unsigned uCount = 0;
			io_uring_for_each_cqe ( &g_tState.m_tRing, uHead, pCqe )
			{
				++uCount;
				uint64_t uData = io_uring_cqe_get_data64 ( pCqe );
				int iRes = pCqe->res;
				if ( uData==STOP_SENTINEL )
				{
					bStop = true;
					continue;
				}
				auto * pComp = reinterpret_cast<Completion_t *> ( uData );
				if ( pComp )
				{
					if ( pComp->m_fnDone )
						pComp->m_fnDone ( ReadResult_t { iRes } );
					delete pComp;
					// Release the inflight slot reserved by SubmitRead.
					g_tState.m_iInflight.fetch_sub ( 1, std::memory_order_acq_rel );
				}
			}
			io_uring_cq_advance ( &g_tState.m_tRing, uCount );

			if ( bStop )
				break;
		}
	}

	// Probe whether the kernel accepts io_uring_setup at all (seccomp/old kernel guard).
	bool ProbeIoUring()
	{
		io_uring tProbe {};
		int iRc = io_uring_queue_init ( 2, &tProbe, 0 );
		if ( iRc<0 )
			return false;
		io_uring_queue_exit ( &tProbe );
		return true;
	}
}

bool IsIoUringAvailable()
{
	static const bool bAvailable = ProbeIoUring();
	return bAvailable;
}

bool StartIoUring ( unsigned uQueueDepth, bool bSQPoll, unsigned uMaxInflight )
{
	if ( g_tState.m_bRunning.load ( std::memory_order_acquire ) )
		return true;

	if ( !IsIoUringAvailable() )
		return false;

	int iRc = -1;
	g_tState.m_bSQPoll = false;

	if ( bSQPoll )
	{
		io_uring_params tParams {};
		tParams.flags = IORING_SETUP_SQPOLL;
		tParams.sq_thread_idle = 1000; // ms before the kernel SQ thread parks when idle
		iRc = io_uring_queue_init_params ( uQueueDepth, &g_tState.m_tRing, &tParams );
		if ( iRc>=0 )
			g_tState.m_bSQPoll = true;
		// else: SQPOLL refused (privileges/kernel) - fall through to normal init.
	}

	if ( iRc<0 )
		iRc = io_uring_queue_init ( uQueueDepth, &g_tState.m_tRing, 0 );

	if ( iRc<0 )
		return false;

	g_tState.m_bRingInit = true;
	// 0 -> the ring depth is the natural inflight ceiling; otherwise honor the cap.
	g_tState.m_uMaxInflight = uMaxInflight ? uMaxInflight : uQueueDepth;
	g_tState.m_iInflight.store ( 0, std::memory_order_release );
	g_tState.m_bRunning.store ( true, std::memory_order_release );
	g_tState.m_tReaper = std::thread ( ReaperLoop );
	return true;
}

bool IoUringUsesSQPoll()
{
	return g_tState.m_bSQPoll;
}

unsigned IoUringInflightCap()
{
	return g_tState.m_bRunning.load ( std::memory_order_acquire ) ? g_tState.m_uMaxInflight : 0;
}

void StopIoUring()
{
	if ( !g_tState.m_bRunning.exchange ( false, std::memory_order_acq_rel ) )
		return;

	// Wake the reaper with a NOP carrying the stop sentinel.
	{
		std::lock_guard<std::mutex> tLock ( g_tState.m_tSubmitMutex );
		io_uring_sqe * pSqe = io_uring_get_sqe ( &g_tState.m_tRing );
		if ( pSqe )
		{
			io_uring_prep_nop ( pSqe );
			io_uring_sqe_set_data64 ( pSqe, STOP_SENTINEL );
			io_uring_submit ( &g_tState.m_tRing );
		}
	}

	if ( g_tState.m_tReaper.joinable() )
		g_tState.m_tReaper.join();

	if ( g_tState.m_bRingInit )
	{
		io_uring_queue_exit ( &g_tState.m_tRing );
		g_tState.m_bRingInit = false;
	}
}

bool SubmitRead ( int iFD, void * pBuf, unsigned iBytes, int64_t iOffset, OnComplete_fn fnDone )
{
	if ( !g_tState.m_bRunning.load ( std::memory_order_acquire ) )
		return false;

	// Reserve an inflight slot up front (the throttle). When too many reads are already
	// in flight, refuse so the caller does a synchronous read instead of piling unbounded
	// pressure on the ring/disk; the reaper releases the slot on completion. The optimistic
	// add-then-check can transiently overshoot under concurrent submitters but is corrected
	// immediately, so the number of actually-submitted reads never exceeds the cap.
	if ( g_tState.m_iInflight.fetch_add ( 1, std::memory_order_acq_rel ) >= (int)g_tState.m_uMaxInflight )
	{
		g_tState.m_iInflight.fetch_sub ( 1, std::memory_order_acq_rel );
		return false;
	}

	auto * pComp = new Completion_t { std::move ( fnDone ) };

	std::lock_guard<std::mutex> tLock ( g_tState.m_tSubmitMutex );
	io_uring_sqe * pSqe = io_uring_get_sqe ( &g_tState.m_tRing );
	if ( !pSqe )
	{
		// Ring full: caller falls back to synchronous read.
		delete pComp;
		g_tState.m_iInflight.fetch_sub ( 1, std::memory_order_acq_rel );
		return false;
	}

	io_uring_prep_read ( pSqe, iFD, pBuf, iBytes, (uint64_t)iOffset );
	io_uring_sqe_set_data64 ( pSqe, reinterpret_cast<uint64_t> ( pComp ) );

	int iRc = io_uring_submit ( &g_tState.m_tRing );
	if ( iRc<0 )
	{
		// Submission failed; the SQE is dropped. Reclaim the completion and the slot.
		delete pComp;
		g_tState.m_iInflight.fetch_sub ( 1, std::memory_order_acq_rel );
		return false;
	}
	return true;
}

} // namespace IoUring

#else // !HAVE_IO_URING

namespace IoUring
{
	bool IsIoUringAvailable() { return false; }
	bool StartIoUring ( unsigned, bool, unsigned ) { return false; }
	bool IoUringUsesSQPoll() { return false; }
	unsigned IoUringInflightCap() { return 0; }
	void StopIoUring() {}
	bool SubmitRead ( int, void *, unsigned, int64_t, OnComplete_fn ) { return false; }
}

#endif // HAVE_IO_URING
