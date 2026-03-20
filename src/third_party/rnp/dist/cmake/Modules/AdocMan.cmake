# Copyright (c) 2021 Ribose Inc.
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

#.adoc:
# add_adoc_man
# -----------
#
# Convert adoc manual page to troff and install it via the custom target.
#
# Parameters
# ^^^^^^^^^^
# Required parameter is source with markdown file. Must have md extension with man category prepended, i.e. something like ${CMAKE_SOURCE_DIR}/src/utility.1.adoc
# DST - optional parameter, which overrides where generated man will be stored.
# If not specified then will be automatically set to ${CMAKE_BINARY_DIR}/src/utility.1
#
# Generated man page will be installed via the target, named man_utility
#

set(ADOCCOMMAND_FOUND 0)
find_program(ADOCCOMMAND_PATH
  NAMES asciidoctor
  DOC "Path to AsciiDoc processor. Used to generate man pages from AsciiDoc."
)

if(NOT EXISTS ${ADOCCOMMAND_PATH})
  set(ADOC_MISSING_MSG "AsciiDoc processor not found, man pages will not be generated. Install asciidoctor or use the CMAKE_PROGRAM_PATH variable.")

  string(TOLOWER "${ENABLE_DOC}" ENABLE_DOC)
  if (ENABLE_DOC STREQUAL "auto")
    message(WARNING ${ADOC_MISSING_MSG})
  elseif(ENABLE_DOC)
    message(FATAL_ERROR ${ADOC_MISSING_MSG})
  endif()
else()
  set(ADOCCOMMAND_FOUND 1)
endif()

function(add_adoc_man SRC COMPONENT_VERSION)
  if (NOT ${ADOCCOMMAND_FOUND})
    return()
  endif()

  cmake_parse_arguments(
    ARGS
    ""
    "DST"
    ""
    ${ARGN}
  )

  set(ADOC_EXT ".adoc")
  get_filename_component(FILE_NAME ${SRC} NAME)

  # The following procedures check against the expected file name
  # pattern: "{name}.{man-number}.adoc", and builds to a
  # destination file "{name}.{man-number}".

  # Check SRC extension
  get_filename_component(END_EXT ${SRC} LAST_EXT)
  string(COMPARE EQUAL ${END_EXT} ${ADOC_EXT} _equal)
  if (NOT _equal)
    message(FATAL_ERROR "SRC must have ${ADOC_EXT} extension.")
  endif()

  # Check man number
  get_filename_component(EXTS ${SRC} EXT)
  string(REGEX MATCH "^\.([1-9])\.+$" _matches ${EXTS})
  set(MAN_NUM ${CMAKE_MATCH_1})
  if (NOT _matches)
    message(FATAL_ERROR "Man file with wrong name pattern: ${FILE_NAME} must be in format {name}.[0-9]${ADOC_EXT}.")
  endif()

  # Set target name
  get_filename_component(TARGET_NAME ${SRC} NAME_WE)
  string(PREPEND TARGET_NAME "man_")

  # Build output path if not specified.
  if(NOT DST)
    get_filename_component(SRC_PREFIX ${SRC} DIRECTORY)

    # Ensure that SRC_PREFIX is within CMAKE_SOURCE_DIR
    if(NOT(SRC_PREFIX MATCHES "^${CMAKE_SOURCE_DIR}"))
      message(FATAL_ERROR "Cannot build DST path as SRC is outside of the CMake sources dir.")
    endif()
    STRING(REGEX REPLACE "^${CMAKE_SOURCE_DIR}/" "" SUBDIR_PATH ${SRC})

    # Strip '.adoc' from the output subpath
    get_filename_component(SUBDIR_PATH_NAME_WLE ${SUBDIR_PATH} NAME_WLE)
    get_filename_component(SUBDIR_PATH_DIRECTORY ${SUBDIR_PATH} DIRECTORY)
    set(DST "${CMAKE_BINARY_DIR}/${SUBDIR_PATH_DIRECTORY}/${SUBDIR_PATH_NAME_WLE}")
  endif()

  # Check conformance of destination file name to pattern
  get_filename_component(FILE_NAME_WE ${SRC} NAME_WE)
  get_filename_component(MAN_FILE_NAME ${DST} NAME)
  if(NOT(MAN_FILE_NAME MATCHES "^${FILE_NAME_WE}.${MAN_NUM}$"))
    message(FATAL_ERROR "File name of a man page must be in the format {name}.{man-number}${ADOC_EXT}.")
  endif()

  add_custom_command(
    OUTPUT ${DST}
    COMMAND ${ADOCCOMMAND_PATH} -b manpage ${SRC} -o ${DST} -a component-version=${COMPONENT_VERSION}
    DEPENDS ${SRC}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Generating man page ${SUBDIR_PATH_DIRECTORY}/${SUBDIR_PATH_NAME_WLE}"
    VERBATIM
  )

  add_custom_target("${TARGET_NAME}" ALL DEPENDS ${DST})
  install(FILES ${DST}
    DESTINATION "${CMAKE_INSTALL_FULL_MANDIR}/man${MAN_NUM}"
    COMPONENT doc
  )
endfunction(add_adoc_man)
