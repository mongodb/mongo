# Copyright (c) 2018 Ribose Inc.
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
# FindGnuPG
# -----------
#
# Find GnuPG executables.
#
# Imported targets
# ^^^^^^^^^^^^^^^^
#
# This module defines the following :prop_tgt:`IMPORTED` targets:
#
# ::
#
#   GnuPG::<COMPONENT>     - the component executable that was requested (default is just 'gpg')
#
## Result variables
# ^^^^^^^^^^^^^^^^
#
# This module always defines the following variables:
#
# ::
#
#   GNUPG_VERSION          - version that was found
#
# Depending on components requested, this module will also define variables like:
#
# ::
#
#   GPG_EXECUTABLE         - path to the gpg executable
#   <COMPONENT>_EXECUTABLE - path to the component executable
#

# helper that will call <utility_name> --version and extract the version string
function(_get_gpg_version utility_name exe_path var_prefix)
  execute_process(
    COMMAND "${exe_path}" --version
    OUTPUT_VARIABLE version
    RESULT_VARIABLE exit_code
    ERROR_QUIET
  )
  if (NOT exit_code)
    string(REGEX MATCH "${utility_name} \\(GnuPG\\) (([0-9]+)\\.([0-9]+)\\.([0-9]+))" version "${version}")
    if (CMAKE_MATCH_1)
      set(${var_prefix}_VERSION "${CMAKE_MATCH_1}" PARENT_SCOPE)
    endif()
  endif()
endfunction()

# default to finding gpg
if (NOT GnuPG_FIND_COMPONENTS)
  set(GnuPG_FIND_COMPONENTS gpg)
endif()

foreach(_comp IN LISTS GnuPG_FIND_COMPONENTS)
  # we also check for an executable with the 2 suffix when appropriate
  set(_names "${_comp}")
  if (_comp STREQUAL "gpg" OR _comp STREQUAL "gpgv")
    if (NOT ${GnuPG_FIND_VERSION})
      set(_names "${_comp}2" ${_comp})
    elseif (${GnuPG_FIND_VERSION} VERSION_GREATER_EQUAL 2.2)
      # 2.2+ defaults to gpg/gpgv, but supports gpg2/gpgv2
      set(_names ${_comp} "${_comp}2")
    elseif(${GnuPG_FIND_VERSION} VERSION_GREATER_EQUAL 2.0)
      # 2.0-2.2 or so used a temporary naming of gpg2/gpgv2
      set(_names "${_comp}2" ${_comp})
    endif()
  endif()
  string(TOUPPER "${_comp}" _comp_upper)
  find_program(${_comp_upper}_EXECUTABLE NAMES ${_names})
  unset(_names)
  mark_as_advanced(${_comp_upper}_EXECUTABLE)

  # if we found an executable, check the version
  if (${_comp_upper}_EXECUTABLE)
    _get_gpg_version(${_comp} ${${_comp_upper}_EXECUTABLE} _${_comp})
    if (_${_comp}_VERSION)
      if (NOT GNUPG_VERSION)
        # this is the first component found, so set the version to match
        set(GNUPG_VERSION ${_${_comp}_VERSION})
      endif()
      # see if the version matches the previous components found
      if(_${_comp}_VERSION VERSION_EQUAL ${GNUPG_VERSION} AND NOT TARGET GnuPG::${_comp})
        add_executable(GnuPG::${_comp} IMPORTED GLOBAL)
        set_target_properties(GnuPG::${_comp} PROPERTIES
          IMPORTED_LOCATION "${${_comp_upper}_EXECUTABLE}"
        )
      endif()
    endif()
    unset(_${_comp}_VERSION)
  endif()

  # mark our components as found or not found
  if (TARGET GnuPG::${_comp})
    set(GnuPG_${_comp}_FOUND TRUE)
  else()
    set(GnuPG_${_comp}_FOUND FALSE)
    unset(${_comp_upper}_EXECUTABLE)
  endif()

  if (GnuPG_FIND_REQUIRED_${_comp})
    list(APPEND _GnuPG_REQUIRED_VARS ${_comp_upper}_EXECUTABLE)
  endif()
endforeach()
unset(_comp)
unset(_comp_upper)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(GnuPG
  REQUIRED_VARS ${_GnuPG_REQUIRED_VARS}
  VERSION_VAR GNUPG_VERSION
  HANDLE_COMPONENTS
)

