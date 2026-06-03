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

namespace
{
	int IoUringPreadCoro ( int iFD, void * pBuf, int iBytes, SphOffset_t iOffset )
	{
		// Outside a coroutine (service tasks, load) or backend down: read synchronously.
		if ( !Threads::Coro::CurrentWorker() || !IoUring::IsIoUringAvailable() )
			return sphPread ( iFD, pBuf, iBytes, iOffset );

		auto tWaker = Threads::CreateWaker(); // waker for the current fiber
		int iResult = 0;
		bool bSubmitted = false;

		// Arm the read *inside* YieldWith, i.e. after the fiber has parked, so the
		// completion (which may fire immediately on the reaper thread) cannot be lost.
		Threads::Coro::YieldWith ( [&] () NO_THREAD_SAFETY_ANALYSIS
		{
			bSubmitted = IoUring::SubmitRead ( iFD, pBuf, (unsigned)iBytes, iOffset,
				[&iResult, tWaker] ( IoUring::ReadResult_t tRes )
				{
					iResult = tRes.m_iRes; // >=0 bytes, <0 is -errno (mirrors pread)
					tWaker.Wake();
				} );

			// Ring full / backend down: nothing will wake us, so wake immediately
			// and let the post-resume code fall back to a synchronous read.
			if ( !bSubmitted )
				tWaker.Wake();
		} );

		if ( !bSubmitted )
			return sphPread ( iFD, pBuf, iBytes, iOffset );

		CSphIOStats * pIOStats = GetIOStats();
		if ( pIOStats && iResult>0 )
		{
			pIOStats->m_iReadOps++;
			pIOStats->m_iReadBytes += iBytes;
		}
		return iResult;
	}
}

void InstallIoUringReadHook()
{
	SetPreadCoroHook ( &IoUringPreadCoro );
}
