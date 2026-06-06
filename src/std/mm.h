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

#pragma once

#include "ints.h"

enum class Mode_e {
	NONE,
	READ,
	WRITE,
	RW,
};

enum class Share_e {
	ANON_PRIVATE,
	ANON_SHARED,
	SHARED,
};

enum class Advise_e {
	// housekeeping (always applied to index maps)
	NOFORK,
	NODUMP,
	// access-pattern and memory-residency hints (advisory; no-op where the MADV_* constant is undefined)
	NORMAL,
	RANDOM,
	SEQUENTIAL,
	WILLNEED,
	DONTNEED,
	HUGEPAGE,
	COLD,
	PAGEOUT,
};

// access intent for an mmaped index component; selects which performance hints mmadviseAccess() applies.
// derived from the component's FileAccess_e mode at the call site (kept here so the low-level fileutils/mm
// layer does not need to depend on indexsettings.h).
enum class MmapAdvise_e {
	NONE,		// legacy: apply only NOFORK/NODUMP housekeeping
	PREREAD,	// resident, prefaulted & scanned set -> WILLNEED
	RANDOM,		// randomly probed, no preread -> RANDOM (suppress default readahead)
};

// advise HUGEPAGE only on maps at least this large, so we do not fragment many small maps
const size_t MMAP_HUGEPAGE_THRESHOLD = 2 * 1024 * 1024; // 2 MiB == one transparent huge page

// runtime switches, set from the searchd config (default on). when g_bMmapAdvise is off, only the
// NOFORK/NODUMP housekeeping hints are applied; g_bMmapHugePages independently gates HUGEPAGE.
extern bool g_bMmapAdvise;
extern bool g_bMmapHugePages;

void* mmalloc ( size_t uSize, Mode_e = Mode_e::RW, Share_e = Share_e::ANON_PRIVATE );
bool mmapvalid ( const void* pMem );
int mmfree ( void* pMem, size_t uSize );
void mmadvise ( void* pMem, size_t uSize, Advise_e = Advise_e::NODUMP );
bool mmlock ( void* pMem, size_t uSize );
bool mmunlock ( void* pMem, size_t uSize );

// apply access-pattern + huge-page hints to a freshly mmaped index region, honoring the global switches.
// a NONE policy or a disabled switch issues no performance hint. always safe (advisory).
void mmadviseAccess ( void* pMem, size_t uSize, MmapAdvise_e eAccess );

// proactively return resident memory for a region known to be cold (COLD then PAGEOUT).
// no-op when g_bMmapAdvise is off or the kernel lacks the hints.
void mmadviseCold ( void* pMem, size_t uSize );