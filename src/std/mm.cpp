//
// Copyright (c) 2017-2026, Manticore Software LTD (https://manticoresearch.com)
// Copyright (c) 2001-2016, Andrew Aksyonoff
// Copyright (c) 2008-2016, Sphinx Technologies Inc
// All rights reserved
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org
//

#include "mm.h"

#include <cassert>

// performance-hint switches; default on, overridden from the searchd config (see searchd.cpp).
// when off, mmadviseAccess()/mmadviseCold() issue no MADV_* performance hint.
bool g_bMmapAdvise = true;
bool g_bMmapHugePages = true;

#if _WIN32
#include <cstdlib>

void* mmalloc ( size_t uSize, Mode_e, Share_e )
{
	return ::malloc ( (size_t)uSize );
}

bool mmapvalid ( const void* pMem )
{
	return pMem != nullptr;
}

int mmfree ( void* pMem, size_t )
{
	assert ( mmapvalid ( pMem ) );
	::free ( pMem );
	return 0;
}

void mmadvise ( void*, size_t, Advise_e ) {}

void mmadviseAccess ( void*, size_t, MmapAdvise_e ) {}

void mmadviseCold ( void*, size_t ) {}

bool mmlock ( void* pMem, size_t uSize )
{
	return VirtualLock ( pMem, uSize ) != 0;
}

bool mmunlock ( void* pMem, size_t uSize )
{
	return VirtualUnlock ( pMem, uSize ) != 0;
}

#else

#include <sys/mman.h>

// couple of helpers
int hwShare ( Share_e eAccess )
{
	switch ( eAccess )
	{
	case Share_e::ANON_PRIVATE: return MAP_ANON | MAP_PRIVATE;
	case Share_e::ANON_SHARED: return MAP_ANON | MAP_SHARED;
	case Share_e::SHARED: return MAP_SHARED;
	}
	return MAP_SHARED;
}

int hwMode ( Mode_e eMode )
{
	switch ( eMode )
	{
	case Mode_e::NONE: return PROT_NONE;
	case Mode_e::READ: return PROT_READ;
	case Mode_e::WRITE: return PROT_WRITE;
	case Mode_e::RW: return PROT_READ | PROT_WRITE;
	}
	return PROT_READ | PROT_WRITE;
}

void* mmalloc ( size_t uSize, Mode_e eMode, Share_e eAccess )
{
	return mmap ( NULL, uSize, hwMode ( eMode ), hwShare ( eAccess ), -1, 0 );
}

bool mmapvalid ( const void* pMem )
{
	return pMem != MAP_FAILED;
}

int mmfree ( void* pMem, size_t uSize )
{
	assert ( mmapvalid ( pMem ) );
	return munmap ( pMem, uSize );
}

void mmadvise ( void* pMem, size_t uSize, Advise_e eAdvise )
{
	switch ( eAdvise )
	{
	case Advise_e::NODUMP:
#ifdef MADV_DONTDUMP
		madvise ( pMem, uSize, MADV_DONTDUMP );
#endif
		break;
	case Advise_e::NOFORK:
		madvise ( pMem, uSize,
#ifdef MADV_DONTFORK
			MADV_DONTFORK
#else
			MADV_NORMAL
#endif
		);
		break;

	// access-pattern and memory-residency hints. each is best-effort: a constant the running
	// kernel header does not define compiles to a no-op, and a failed madvise() is ignored.
	case Advise_e::NORMAL:
#ifdef MADV_NORMAL
		madvise ( pMem, uSize, MADV_NORMAL );
#endif
		break;
	case Advise_e::RANDOM:
#ifdef MADV_RANDOM
		madvise ( pMem, uSize, MADV_RANDOM );
#endif
		break;
	case Advise_e::SEQUENTIAL:
#ifdef MADV_SEQUENTIAL
		madvise ( pMem, uSize, MADV_SEQUENTIAL );
#endif
		break;
	case Advise_e::WILLNEED:
#ifdef MADV_WILLNEED
		madvise ( pMem, uSize, MADV_WILLNEED );
#endif
		break;
	case Advise_e::DONTNEED:
#ifdef MADV_DONTNEED
		madvise ( pMem, uSize, MADV_DONTNEED );
#endif
		break;
	case Advise_e::HUGEPAGE:
#ifdef MADV_HUGEPAGE
		madvise ( pMem, uSize, MADV_HUGEPAGE );
#endif
		break;
	case Advise_e::COLD:
#ifdef MADV_COLD
		madvise ( pMem, uSize, MADV_COLD );
#endif
		break;
	case Advise_e::PAGEOUT:
#ifdef MADV_PAGEOUT
		madvise ( pMem, uSize, MADV_PAGEOUT );
#endif
		break;
	}
}

void mmadviseAccess ( void* pMem, size_t uSize, MmapAdvise_e eAccess )
{
	if ( !g_bMmapAdvise || eAccess==MmapAdvise_e::NONE )
		return;

	switch ( eAccess )
	{
	case MmapAdvise_e::PREREAD:
		mmadvise ( pMem, uSize, Advise_e::WILLNEED );
		break;
	case MmapAdvise_e::RANDOM:
		mmadvise ( pMem, uSize, Advise_e::RANDOM );
		break;
	case MmapAdvise_e::NONE:
		break;
	}

	if ( g_bMmapHugePages && uSize>=MMAP_HUGEPAGE_THRESHOLD )
		mmadvise ( pMem, uSize, Advise_e::HUGEPAGE );
}

void mmadviseCold ( void* pMem, size_t uSize )
{
	if ( !g_bMmapAdvise )
		return;

	// COLD deactivates the pages (cheap to reclaim under pressure); PAGEOUT asks the kernel to
	// reclaim them now so RSS drops immediately. both are no-ops on kernels < 5.4.
	mmadvise ( pMem, uSize, Advise_e::COLD );
	mmadvise ( pMem, uSize, Advise_e::PAGEOUT );
}

bool mmlock ( void* pMem, size_t uSize )
{
	return mlock ( pMem, uSize ) == 0;
}

bool mmunlock ( void* pMem, size_t uSize )
{
	return munlock ( pMem, uSize ) == 0;
}

#endif // _WIN32