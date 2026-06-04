cmake_minimum_required ( VERSION 3.17 FATAL_ERROR )

set ( ROARINGBITMAP_GITHUB "https://github.com/RoaringBitmap/CRoaring/archive/refs/tags/v4.3.2.tar.gz" )
set ( ROARINGBITMAP_BUNDLE "${LIBS_BUNDLE}/roaring-v4.3.2.tar.gz" )
set ( ROARINGBITMAP_SRC_MD5 "9ad3047cd74e5a3562c30f7c8a606373" )

include ( update_bundle )

# try to find quietly (will work most of the times)
find_package ( roaring QUIET CONFIG )
return_if_target_found ( roaring::roaring "found ready (no need to build)" )

# not found. Populate and prepare sources
select_nearest_url ( ROARINGBITMAP_PLACE roaring ${ROARINGBITMAP_BUNDLE} ${ROARINGBITMAP_GITHUB} )
fetch_and_check ( roaring ${ROARINGBITMAP_PLACE} ${ROARINGBITMAP_SRC_MD5} ROARINGBITMAP_SRC )

# build external project
# amd64-only: keep CRoaring's AVX2/AVX-512 code paths enabled. CRoaring selects them at
# runtime (croaring_hardware_support), so the resulting binary still runs on CPUs without
# AVX while using the vectorized popcount/and/or/intersect on capable hardware. NEON stays
# disabled - it is ARM-only and never compiled on x86_64 anyway.
get_build ( ROARINGBITMAP_BUILD roaring )
external_build ( roaring ROARINGBITMAP_SRC ROARINGBITMAP_BUILD ROARING_EXCEPTIONS=0 ROARING_USE_CPM=0 ENABLE_ROARING_TESTS=0 ROARING_DISABLE_NEON=1 )

# now it should find
find_package ( roaring REQUIRED CONFIG )
return_if_target_found ( roaring::roaring "was built and saved" )
