# Vendored liburing build for environments where the (cross) sysroot has no liburing
# (the Manticore cross toolchain does not ship it). liburing uses an autotools-style
# ./configure + make, so we can't reuse external_build (which is CMake-only); we drive
# its native build via ExternalProject and expose a liburing::liburing target.
#
# Used as a fallback by the WITH_LIBURING wiring: system liburing (Findliburing) is
# preferred; this kicks in only on Linux when no system liburing is present.

cmake_minimum_required ( VERSION 3.17 FATAL_ERROR )
include ( ExternalProject )

set ( LIBURING_VERSION "2.5" )
set ( LIBURING_URL "https://github.com/axboe/liburing/archive/refs/tags/liburing-${LIBURING_VERSION}.tar.gz" )

set ( _liburing_root "${CMAKE_BINARY_DIR}/liburing-ep" )
set ( _liburing_install "${_liburing_root}/install" )
set ( _liburing_lib "${_liburing_install}/lib/liburing.a" )
set ( _liburing_inc "${_liburing_install}/include" )

# Cross-aware flags for liburing's own configure/make.
set ( _liburing_cflags "-O2 -fPIC" )
if ( CMAKE_SYSROOT )
	set ( _liburing_cflags "${_liburing_cflags} --sysroot=${CMAKE_SYSROOT}" )
endif ()

ExternalProject_Add ( liburing_ep
	URL "${LIBURING_URL}"
	DOWNLOAD_EXTRACT_TIMESTAMP ON
	PREFIX "${_liburing_root}"
	BUILD_IN_SOURCE 1
	# liburing's configure honors --cc; build only the static lib (no shared, no examples/tests).
	CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=${_liburing_install} --cc=${CMAKE_C_COMPILER}
	BUILD_COMMAND make -C <SOURCE_DIR>/src "CC=${CMAKE_C_COMPILER}" "CFLAGS=${_liburing_cflags}" ENABLE_SHARED=0
	INSTALL_COMMAND make -C <SOURCE_DIR>/src install prefix=${_liburing_install} ENABLE_SHARED=0
	BUILD_BYPRODUCTS "${_liburing_lib}"
	LOG_DOWNLOAD 1 LOG_CONFIGURE 1 LOG_BUILD 1 LOG_INSTALL 1
)

# The include dir must exist at configure time for INTERFACE_INCLUDE_DIRECTORIES.
file ( MAKE_DIRECTORY "${_liburing_inc}" )

# Expose as liburing::liburing. Use a real INTERFACE lib (imported targets can't carry
# add_dependencies), so consumers both build liburing_ep first and link the static lib.
add_library ( liburing_vendored INTERFACE )
add_dependencies ( liburing_vendored liburing_ep )
target_include_directories ( liburing_vendored INTERFACE "${_liburing_inc}" )
target_link_libraries ( liburing_vendored INTERFACE "${_liburing_lib}" )
add_library ( liburing::liburing ALIAS liburing_vendored )

set ( liburing_FOUND TRUE )
