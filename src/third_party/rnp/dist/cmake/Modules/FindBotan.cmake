# Copyright (c) 2018-2020 Ribose Inc.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
# TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
# BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.

#.rst:
# FindBotan
# -----------
#
# Find the botan-2 or botan-3 library.
#
# IMPORTED Targets
# ^^^^^^^^^^^^^^^^
#
# This module defines :prop_tgt:`IMPORTED` targets:
#
# ``Botan::Botan``
#   The botan-2 or botan-3 library, if found.
#
# Result variables
# ^^^^^^^^^^^^^^^^
#
# This module defines the following variables:
#
# ::
#
#   BOTAN_FOUND          - true if the headers and library were found
#   BOTAN_INCLUDE_DIRS   - where to find headers
#   BOTAN_LIBRARIES      - list of libraries to link
#   Botan_VERSION        - library version that was found, if any
#
# Hints
# ^^^^^
#
# These variables may be set to control search behaviour:
#
# ``BOTAN_ROOT_DIR``
#   Set to the root directory of the Botan installation.
#

# use pkg-config to get the directories and then use these values
# in the find_path() and find_library() calls

find_package(PkgConfig QUIET)

# Search for the version 2 first unless version 3 requested
if(NOT "${Botan_FIND_VERSION_MAJOR}" EQUAL "3")
  pkg_check_modules(PC_BOTAN QUIET botan-2)
  set(_suffixes "botan-2" "botan-3")
  set(_names "botan-2" "libbotan-2" "botan-3" "libbotan-3")
else()
  set(_suffixes "botan-3")
  set(_names "botan-3" "libbotan-3")
endif()
if(NOT PC_BOTAN_FOUND)
  pkg_check_modules(PC_BOTAN QUIET botan-3)
endif()

if(DEFINED BOTAN_ROOT_DIR)
  set(_hints_include "${BOTAN_ROOT_DIR}/include")
  set(_hints_lib "${BOTAN_ROOT_DIR}/lib")
endif()
if(DEFINED ENV{BOTAN_ROOT_DIR})
  list(APPEND _hints_include "$ENV{BOTAN_ROOT_DIR}/include")
  list(APPEND _hints_lib "$ENV{BOTAN_ROOT_DIR}/lib")
endif()

# Append PC_* stuff only if BOTAN_ROOT_DIR is not specified
if(NOT _hints_include)
  list(APPEND _hints_include ${PC_BOTAN_INCLUDEDIR} ${PC_BOTAN_INCLUDE_DIRS})
  list(APPEND _hints_lib ${PC_BOTAN_LIBDIR} ${PC_BOTAN_LIBRARY_DIRS})
else()
  set(_no_def_path "NO_DEFAULT_PATH")
endif()

# find the headers
find_path(BOTAN_INCLUDE_DIR
  NAMES botan/version.h
  HINTS
    ${_hints_include}
  PATH_SUFFIXES ${_suffixes}
  ${_no_def_path}
)

# find the library
if(MSVC)
  find_library(BOTAN_LIBRARY
    NAMES botan ${_names}
    HINTS
      ${_hints_lib}
    ${_no_def_path}
  )
else()
  find_library(BOTAN_LIBRARY
    NAMES
      ${_names}
    HINTS
      ${_hints_lib}
    ${_no_def_path}
  )
endif()

# determine the version
if(BOTAN_INCLUDE_DIR AND EXISTS "${BOTAN_INCLUDE_DIR}/botan/build.h")
    file(STRINGS "${BOTAN_INCLUDE_DIR}/botan/build.h" botan_version_str
      REGEX "^#define[\t ]+(BOTAN_VERSION_[A-Z]+)[\t ]+[0-9]+")

    string(REGEX REPLACE ".*#define[\t ]+BOTAN_VERSION_MAJOR[\t ]+([0-9]+).*"
           "\\1" _botan_version_major "${botan_version_str}")
    string(REGEX REPLACE ".*#define[\t ]+BOTAN_VERSION_MINOR[\t ]+([0-9]+).*"
           "\\1" _botan_version_minor "${botan_version_str}")
    string(REGEX REPLACE ".*#define[\t ]+BOTAN_VERSION_PATCH[\t ]+([0-9]+).*"
           "\\1" _botan_version_patch "${botan_version_str}")
    set(Botan_VERSION "${_botan_version_major}.${_botan_version_minor}.${_botan_version_patch}"
                       CACHE INTERNAL "The version of Botan which was detected")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Botan
  REQUIRED_VARS BOTAN_LIBRARY BOTAN_INCLUDE_DIR
  VERSION_VAR Botan_VERSION
)

if (BOTAN_FOUND)
  set(BOTAN_INCLUDE_DIRS ${BOTAN_INCLUDE_DIR} ${PC_BOTAN_INCLUDE_DIRS})
  set(BOTAN_LIBRARIES ${BOTAN_LIBRARY})
endif()

if (BOTAN_FOUND AND NOT TARGET Botan::Botan)
  # create the new library target
  add_library(Botan::Botan UNKNOWN IMPORTED)
  # set the required include dirs for the target
  if (BOTAN_INCLUDE_DIRS)
    set_target_properties(Botan::Botan
      PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${BOTAN_INCLUDE_DIRS}"
    )
  endif()
  # set the required libraries for the target
  if (EXISTS "${BOTAN_LIBRARY}")
    set_target_properties(Botan::Botan
      PROPERTIES
        IMPORTED_LINK_INTERFACE_LANGUAGES "C"
        IMPORTED_LOCATION "${BOTAN_LIBRARY}"
    )
  endif()
endif()

mark_as_advanced(BOTAN_INCLUDE_DIR BOTAN_LIBRARY)
