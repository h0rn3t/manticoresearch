// Unit-compile + behavior test for the standalone io_uring backend (src/iouring.*).
// Exercises StartIoUring -> SubmitRead -> reaper callback -> StopIoUring.
// Build: cc iouring_unit.cpp ../../../../src/iouring.cpp -DHAVE_IO_URING=1 -luring -lstdc++ -lpthread
#include "../../../../src/iouring.h"

#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <atomic>

int main()
{
	if ( !IoUring::IsIoUringAvailable() )
	{
		fprintf ( stderr, "io_uring not available in this environment\n" );
		return 2;
	}
	if ( !IoUring::StartIoUring ( 64 ) )
	{
		fprintf ( stderr, "StartIoUring failed\n" );
		return 3;
	}

	int iFd = open ( "/etc/hostname", O_RDONLY );
	if ( iFd<0 ) { perror ( "open" ); return 4; }

	char dBuf[256];
	memset ( dBuf, 0, sizeof ( dBuf ) );

	std::mutex tMtx;
	std::condition_variable tCv;
	bool bDone = false;
	int iBytes = -777;

	bool bSubmitted = IoUring::SubmitRead ( iFd, dBuf, sizeof ( dBuf )-1, 0,
		[&] ( IoUring::ReadResult_t tRes )
		{
			std::lock_guard<std::mutex> l ( tMtx );
			iBytes = tRes.m_iRes;
			bDone = true;
			tCv.notify_one();
		} );

	if ( !bSubmitted ) { fprintf ( stderr, "SubmitRead returned false\n" ); return 5; }

	{
		std::unique_lock<std::mutex> l ( tMtx );
		if ( !tCv.wait_for ( l, std::chrono::seconds ( 5 ), [&]{ return bDone; } ) )
		{
			fprintf ( stderr, "timeout waiting for completion\n" );
			return 6;
		}
	}

	if ( iBytes<0 ) { fprintf ( stderr, "read failed: %s\n", strerror ( -iBytes ) ); return 7; }

	printf ( "OK  backend read %d bytes via reaper callback: \"%.*s\"\n",
		iBytes, iBytes>40?40:iBytes, dBuf );

	// Submit a few concurrent reads to exercise the submit mutex + reaper drain.
	std::atomic<int> iLeft { 8 };
	std::mutex tM2; std::condition_variable tCv2; bool bAll = false;
	char dScratch[8][64];
	for ( int i=0; i<8; ++i )
	{
		IoUring::SubmitRead ( iFd, dScratch[i], 16, 0,
			[&] ( IoUring::ReadResult_t )
			{
				if ( iLeft.fetch_sub ( 1 )==1 )
				{
					std::lock_guard<std::mutex> l ( tM2 );
					bAll = true; tCv2.notify_one();
				}
			} );
	}
	{
		std::unique_lock<std::mutex> l ( tM2 );
		if ( !tCv2.wait_for ( l, std::chrono::seconds ( 5 ), [&]{ return bAll; } ) )
		{ fprintf ( stderr, "timeout on concurrent batch\n" ); return 8; }
	}
	printf ( "OK  8 concurrent reads all completed via reaper\n" );

	IoUring::StopIoUring();
	close ( iFd );
	printf ( "ALL OK: io_uring backend module works (start/submit/reap/stop)\n" );
	return 0;
}
