# Copyright (c) 2018, 2024 Ribose Inc.
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
# FindJSON-C
# -----------
#
# Find the json-c library.
#
# IMPORTED Targets
# ^^^^^^^^^^^^^^^^
#
# This module defines :prop_tgt:`IMPORTED` targets:
#
# ``JSON-C::JSON-C``
#   The json-c library, if found.
#
# Result variables
# ^^^^^^^^^^^^^^^^
#
# This module defines the following variables:
#
# ::
#
#   JSON-C_FOUND          - true if the headers and library were found
#   JSON-C_INCLUDE_DIRS   - where to find headers
#   JSON-C_LIBRARIES      - list of libraries to link
#   JSON-C_VERSION        - library version that was found, if any

# use pkg-config to get the directories and then use these values
# in the find_path() and find_library() calls
find_package(PkgConfig QUIET)
pkg_check_modules(PC_JSON-C QUIET json-c)

# RHEL-based systems may have json-c12
if (NOT PC_JSON-C_FOUND)
  pkg_check_modules(PC_JSON-C QUIET json-c12)
endif()

# ..or even json-c13, accompanied by non-develop json-c (RHEL 8 ubi)
if (NOT PC_JSON-C_FOUND)
  pkg_check_modules(PC_JSON-C QUIET json-c13)
endif()

# find the headers
find_path(JSON-C_INCLUDE_DIR
  NAMES json_c_version.h
  HINTS
    ${PC_JSON-C_INCLUDEDIR}
    ${PC_JSON-C_INCLUDE_DIRS}
  PATH_SUFFIXES json-c json-c12 json-c13
)

# find the library
find_library(JSON-C_LIBRARY
  NAMES json-c libjson-c json-c12 libjson-c12 json-c13 libjson-c13
  HINTS
    ${PC_JSON-C_LIBDIR}
    ${PC_JSON-C_LIBRARY_DIRS}
)

# determine the version
if(PC_JSON-C_VERSION)
    set(JSON-C_VERSION ${PC_JSON-C_VERSION})
elseif(JSON-C_INCLUDE_DIR AND EXISTS "${JSON-C_INCLUDE_DIR}/json_c_version.h")
    file(STRINGS "${JSON-C_INCLUDE_DIR}/json_c_version.h" _json-c_version_h
      REGEX "^#define[\t ]+JSON_C_VERSION[\t ]+\"[^\"]*\"$")

    string(REGEX REPLACE ".*#define[\t ]+JSON_C_VERSION[\t ]+\"([^\"]*)\".*"
      "\\1" _json-c_version_str "${_json-c_version_h}")
    set(JSON-C_VERSION "${_json-c_version_str}"
                       CACHE INTERNAL "The version of json-c which was detected")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(JSON-C
  REQUIRED_VARS JSON-C_LIBRARY JSON-C_INCLUDE_DIR JSON-C_VERSION
  VERSION_VAR JSON-C_VERSION
)

if (JSON-C_FOUND)
  set(JSON-C_INCLUDE_DIRS ${JSON-C_INCLUDE_DIR} ${PC_JSON-C_INCLUDE_DIRS})
  set(JSON-C_LIBRARIES ${JSON-C_LIBRARY})
endif()

if (JSON-C_FOUND AND NOT TARGET JSON-C::JSON-C)
  # create the new library target
  add_library(JSON-C::JSON-C UNKNOWN IMPORTED)
  # set the required include dirs for the target
  if (JSON-C_INCLUDE_DIRS)
    set_target_properties(JSON-C::JSON-C
      PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${JSON-C_INCLUDE_DIRS}"
    )
  endif()
  # set the required libraries for the target
  if (EXISTS "${JSON-C_LIBRARY}")
    set_target_properties(JSON-C::JSON-C
      PROPERTIES
        IMPORTED_LINK_INTERFACE_LANGUAGES "C"
        IMPORTED_LOCATION "${JSON-C_LIBRARY}"
    )
  endif()
endif()

mark_as_advanced(JSON-C_INCLUDE_DIR JSON-C_LIBRARY)
