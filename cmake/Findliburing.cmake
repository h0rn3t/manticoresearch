#.rst:
# Findliburing
# --------------
#
# Find liburing library and headers (Linux io_uring userspace).
#
# The module defines the following variables:
#
# ::
#
#   liburing_FOUND        - true if liburing was found
#   LIBURING_INCLUDE_DIRS - include search path
#   LIBURING_LIBRARY      - library to link
#
# The module checks also these variables:
#   WITH_LIBURING_ROOT     - the full path to liburing prefix (highest priority)
#   WITH_LIBURING_INCLUDES - where to find the header files
#   WITH_LIBURING_LIBS     - where to search for the lib
#
# Produces the imported target liburing::liburing.

#=============================================================================
# Copyright 2017-2026, Manticore Software LTD (https://manticoresearch.com)
#
# Distributed under the OSI-approved BSD License (the "License");
# see accompanying file Copyright.txt for details.
#
# This software is distributed WITHOUT ANY WARRANTY; without even the
# implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
# See the License for more information.
#=============================================================================

# liburing is Linux-only; never even try elsewhere.
if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
	set ( liburing_FOUND FALSE )
	return ()
endif ()

# Explicit root wins over everything.
if (EXISTS "${WITH_LIBURING_ROOT}/include/liburing.h")
	set ( LIBURING_INCLUDE_DIRS "${WITH_LIBURING_ROOT}/include" )
	find_library ( LIBURING_LIBRARY NAMES uring liburing HINTS "${WITH_LIBURING_ROOT}/lib" NO_DEFAULT_PATH )
else ()
	# Try pkg-config first (liburing ships a .pc file).
	find_package ( PkgConfig QUIET )
	if (PKG_CONFIG_FOUND)
		pkg_check_modules ( PC_LIBURING QUIET liburing )
	endif ()

	if (EXISTS "${WITH_LIBURING_INCLUDES}")
		set ( LIBURING_INCLUDE_DIRS "${WITH_LIBURING_INCLUDES}" )
	else ()
		find_path ( LIBURING_INCLUDE_DIRS NAMES liburing.h
				HINTS ${PC_LIBURING_INCLUDEDIR} ${PC_LIBURING_INCLUDE_DIRS}
				PATHS /usr/include /usr/local/include )
	endif ()

	set ( CMAKE_FIND_LIBRARY_SUFFIXES .a .so )
	find_library ( LIBURING_LIBRARY NAMES uring liburing
			HINTS ${PC_LIBURING_LIBDIR} ${PC_LIBURING_LIBRARY_DIRS}
			PATHS
			${WITH_LIBURING_LIBS}
			/usr/lib/x86_64-linux-gnu
			/usr/lib/aarch64-linux-gnu
			/usr/lib64
			/usr/local/lib64
			/usr/lib
			/usr/local/lib )
endif ()

mark_as_advanced ( LIBURING_INCLUDE_DIRS LIBURING_LIBRARY )

include ( FindPackageHandleStandardArgs )
find_package_handle_standard_args ( liburing REQUIRED_VARS LIBURING_INCLUDE_DIRS LIBURING_LIBRARY )

if (liburing_FOUND AND NOT TARGET liburing::liburing)
	add_library ( liburing::liburing UNKNOWN IMPORTED )
	set_target_properties ( liburing::liburing PROPERTIES
			IMPORTED_LOCATION "${LIBURING_LIBRARY}"
			INTERFACE_INCLUDE_DIRECTORIES "${LIBURING_INCLUDE_DIRS}"
			)
endif ()
