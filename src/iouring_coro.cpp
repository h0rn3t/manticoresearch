//
// Copyright (c) 2026, Manticore Software LTD (https://manticoresearch.com)
// All rights reserved
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

// Coroutine glue for io_uring async disk reads. Lives in the searchd-only object
// library (lsearchd), so the coroutine + liburing graph is NOT pulled into tools
// like indexer. It installs a hook into fileio's sphPreadCoro: when running inside
// a coroutine with the io_uring backend up, a read is submitted to the ring and the
// fiber is parked until the reaper wakes it; otherwise it falls back to sphPread.

#include "fileio.h"
#include "coroutine.h"
#include "iouring.h"
#include "sphinxint.h"

#include <memory>

namespace
{
	// Read outcome lives on the heap (shared_ptr), NOT on the fiber stack. The
	// completion fired by the reaper thread can wake (and thus resume) the fiber
	// while the YieldWith handler is still executing; if the handler or completion
	// touched fiber-stack variables they would read a popped stack -> crash. So
	// everything the async callbacks need is captured by value / via this state.
	struct ReadState_t
	{
		int		m_iResult = 0;		///< >=0 bytes, <0 is -errno (mirrors pread)
		bool	m_bSubmitted = false; ///< false => caller must fall back to sphPread
	};

	int IoUringPreadCoro ( int iFD, void * pBuf, int iBytes, SphOffset_t iOffset )
	{
		// Outside a coroutine (service tasks, load) or backend down: read synchronously.
		if ( !Threads::Coro::CurrentWorker() || !IoUring::IsIoUringAvailable() )
			return sphPread ( iFD, pBuf, iBytes, iOffset );

		auto pState = std::make_shared<ReadState_t>();
		auto tWaker = Threads::CreateWaker(); // waker for the current fiber

		// Arm the read inside YieldWith (after the fiber parks). Capture everything
		// BY VALUE: the handler must not reference the fiber stack, because the
		// completion may resume the fiber before the handler returns.
		Threads::Coro::YieldWith ( [pState, tWaker, iFD, pBuf, iBytes, iOffset] () NO_THREAD_SAFETY_ANALYSIS
		{
			bool bOk = IoUring::SubmitRead ( iFD, pBuf, (unsigned)iBytes, iOffset,
				[pState, tWaker] ( IoUring::ReadResult_t tRes )
				{
					pState->m_iResult = tRes.m_iRes;
					pState->m_bSubmitted = true;
					tWaker.Wake();
				} );

			// Ring full / backend down: nothing will wake us, so wake immediately
			// and let the post-resume code fall back to a synchronous read.
			if ( !bOk )
			{
				pState->m_bSubmitted = false;
				tWaker.Wake();
			}
		} );

		if ( !pState->m_bSubmitted )
			return sphPread ( iFD, pBuf, iBytes, iOffset );

		CSphIOStats * pIOStats = GetIOStats();
		if ( pIOStats && pState->m_iResult>0 )
		{
			pIOStats->m_iReadOps++;
			pIOStats->m_iReadBytes += iBytes;
		}
		return pState->m_iResult;
	}
}

void InstallIoUringReadHook()
{
	SetPreadCoroHook ( &IoUringPreadCoro );
}
