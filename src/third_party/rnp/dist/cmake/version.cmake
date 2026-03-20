# Copyright (c) 2018-2021 Ribose Inc.
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

# desired length of commit hash
set(GIT_REV_LEN 7)

# call git, store output in var (can fail)
macro(_git var)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" ${ARGN}
    WORKING_DIRECTORY "${source_dir}"
    RESULT_VARIABLE _git_ec
    OUTPUT_VARIABLE ${var}
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
endmacro()

function(extract_version_info version var_prefix)
  # extract the main components
  #   v1.9.0-3-g5b92266+1546836556
  #   v1.9.0-3-g5b92266-dirty+1546836556
  string(REGEX MATCH "^v?([0-9]+\\.[0-9]+\\.[0-9]+)(-([0-9]+)-g([0-9a-f]+)(-dirty)?)?(\\+([0-9]+))?$" matches "${version}")
  if (NOT matches)
    message(FATAL_ERROR "Failed to extract version components from ${version}.")
  endif()
  set(${var_prefix}_VERSION "${CMAKE_MATCH_1}" PARENT_SCOPE) # 1.9.0
  if (NOT CMAKE_MATCH_3)
    set(CMAKE_MATCH_3 "0")
  endif()
  set(${var_prefix}_VERSION_NCOMMITS "${CMAKE_MATCH_3}" PARENT_SCOPE) # 3
  if (NOT CMAKE_MATCH_4)
    set(CMAKE_MATCH_4 "0")
  endif()
  set(${var_prefix}_VERSION_GIT_REV "${CMAKE_MATCH_4}" PARENT_SCOPE) # 5b92266
  if (CMAKE_MATCH_5 STREQUAL "-dirty")
    set(${var_prefix}_VERSION_IS_DIRTY TRUE PARENT_SCOPE)
  else()
    set(${var_prefix}_VERSION_IS_DIRTY FALSE PARENT_SCOPE)
  endif()
  # timestamp is optional, default to 0
  if (NOT CMAKE_MATCH_7)
    set(CMAKE_MATCH_7 "0")
  endif()
  set(${var_prefix}_VERSION_COMMIT_TIMESTAMP "${CMAKE_MATCH_7}" PARENT_SCOPE) # 1546836556
endfunction()

function(determine_version source_dir var_prefix)
  set(has_release_tag NO)
  set(has_version_txt NO)
  set(local_prefix "_determine_ver")
  # find out base version via version.txt
  set(base_version "0.0.0")
  if (EXISTS "${source_dir}/version.txt")
    set(has_version_txt YES)
    file(STRINGS "${source_dir}/version.txt" version_file)
    extract_version_info("${version_file}" "${local_prefix}")
    set(base_version "${${local_prefix}_VERSION}")
    message(STATUS "Found version.txt with ${version_file}")
  else()
    message(STATUS "Found no version.txt.")
  endif()
  # for GIT_EXECUTABLE
  find_package(Git)
  # get a description of the version, something like:
  #   v1.9.1-0-g38ffe82        (a tagged release)
  #   v1.9.1-0-g38ffe82-dirty  (a tagged release with local modifications)
  #   v1.9.0-3-g5b92266        (post-release snapshot)
  #   v1.9.0-3-g5b92266-dirty  (post-release snapshot with local modifications)
  _git(version describe --abbrev=${GIT_REV_LEN} --match "v[0-9]*" --long --dirty)
  if (NOT _git_ec EQUAL 0)
    # no annotated tags, fake one
    message(STATUS "Found no annotated tags.")
    _git(revision rev-parse --short=${GIT_REV_LEN} --verify HEAD)
    if (_git_ec EQUAL 0)
      set(version "v${base_version}-0-g${revision}")
      # check if dirty (this won't detect untracked files, but should be ok)
      _git(changes diff-index --quiet HEAD --)
      if (NOT _git_ec EQUAL 0)
        string(APPEND version "-dirty")
      endif()
      # append the commit timestamp of the most recent commit (only
      # in non-release branches -- typically master)
      _git(commit_timestamp show -s --format=%ct)
      if (_git_ec EQUAL 0)
        string(APPEND version "+${commit_timestamp}")
      endif()
    elseif(has_version_txt)
      # Nothing to get from git - so use version.txt completely
      set(version "${version_file}")
    else()
      # Sad case - no git, no version.txt
      set(version "v${base_version}")
    endif()
  else()
    set(has_release_tag YES)
    message(STATUS "Found annotated tag ${version}")
  endif()
  extract_version_info("${version}" "${local_prefix}")
  if ("${has_version_txt}" AND NOT ${base_version} STREQUAL ${local_prefix}_VERSION)
    message(WARNING "Tagged version ${${local_prefix}_VERSION} doesn't match one from the version.txt: ${base_version}")
    if (${base_version} VERSION_GREATER ${local_prefix}_VERSION)
      set(${local_prefix}_VERSION ${base_version})
    endif()
  endif()
  foreach(suffix VERSION VERSION_NCOMMITS VERSION_GIT_REV VERSION_IS_DIRTY VERSION_COMMIT_TIMESTAMP)
    if (NOT DEFINED ${local_prefix}_${suffix})
      message(FATAL_ERROR "Unable to determine version.")
    endif()
    set(${var_prefix}_${suffix} "${${local_prefix}_${suffix}}" PARENT_SCOPE)
    message(STATUS "${var_prefix}_${suffix}: ${${local_prefix}_${suffix}}")
  endforeach()
  # Set VERSION_SUFFIX and VERSION_FULL. When making changes, be aware that
  # this is used in packaging as well and will affect ordering.
  # | state                 | version_full                |
  # |-----------------------------------------------------|
  # | exact tag             | 0.9.0                       |
  # | exact tag, dirty      | 0.9.0+git20180604           |
  # | after tag             | 0.9.0+git20180604.1.085039f |
  # | no tag, version.txt   | 0.9.0+git20180604.2ee02af   |
  # | no tag, no version.txt| 0.0.0+git20180604.2ee02af   |
  string(TIMESTAMP date "%Y%m%d" UTC)
  set(version_suffix "")
  if (NOT ${local_prefix}_VERSION_NCOMMITS EQUAL 0)
    # 0.9.0+git20150604.4.289818b
    string(APPEND version_suffix "+git${date}.${${local_prefix}_VERSION_NCOMMITS}.${${local_prefix}_VERSION_GIT_REV}")
  elseif ((NOT has_release_tag) AND ((NOT has_version_txt) OR ("${base_version}" STREQUAL "0.0.0") OR (NOT "${revision}" STREQUAL "")))
    # 0.9.0+git20150604.289818b
    string(APPEND version_suffix "+git${date}.${${local_prefix}_VERSION_GIT_REV}")
  elseif(${local_prefix}_VERSION_IS_DIRTY)
    # 0.9.0+git20150604
    string(APPEND version_suffix "+git${date}")
  endif()
  set(version_full "${${local_prefix}_VERSION}${version_suffix}")
  # set the results
  set(${var_prefix}_VERSION_SUFFIX "${version_suffix}" PARENT_SCOPE)
  set(${var_prefix}_VERSION_FULL "${version_full}" PARENT_SCOPE)
  # for informational purposes
  message(STATUS "${var_prefix}_VERSION_SUFFIX: ${version_suffix}")
  message(STATUS "${var_prefix}_VERSION_FULL: ${version_full}")
endfunction()

