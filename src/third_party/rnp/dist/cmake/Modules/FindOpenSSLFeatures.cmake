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

#.rst:
# FindOpenSSLFeatures
# -----------
#
# Find OpenSSL features: supported hashes, ciphers, curves and public-key algorithms.
# Requires FindOpenSSL to be included first, and C compiler to be set as module
# compiles and executes program which do checks against installed OpenSSL library.
#
# Result variables
# ^^^^^^^^^^^^^^^^
#
# This module defines the following variables:
#
# ::
#
#   OPENSSL_SUPPORTED_HASHES    - list of the supported hash algorithms
#   OPENSSL_SUPPORTED_CIPHERS   - list of the supported ciphers
#   OPENSSL_SUPPORTED_CURVES    - list of the supported elliptic curves
#   OPENSSL_SUPPORTED_PUBLICKEY - list of the supported public-key algorithms
#   OPENSSL_SUPPORTED_FEATURES  - all previous lists, glued together
#
# Functions
# ^^^^^^^^^
# OpenSSLHasFeature(FEATURE <VARIABLE>)
# Check whether OpenSSL has corresponding feature (hash/curve/public-key algorithm name, elliptic curve).
# Result is stored in VARIABLE as boolean value, i.e. TRUE or FALSE
#
if (NOT OPENSSL_FOUND)
  message(FATAL_ERROR "OpenSSL is not found. Please make sure that you call find_package(OpenSSL) first.")
endif()

message(STATUS "Querying OpenSSL features")

# Copy and build findopensslfeatures.c in fossl-build subfolder.
set(_fossl_work_dir "${CMAKE_BINARY_DIR}/fossl")
file(MAKE_DIRECTORY "${_fossl_work_dir}")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/findopensslfeatures.c"
  DESTINATION "${_fossl_work_dir}"
)
# As it's short enough let's keep it here.
# Reuse OPENSSL parameters from the upstream project
# otherwise  there is a good chance to find another instance of openssl
# We assume that OpenSSL root is one level up openssl include directory
# This does not look as a good solution, however it is the only one that
# works with all Windows configuration options

message(STATUS "Using OpenSSL root directory at ${OPENSSL_INCLUDE_DIR}/..")

file(WRITE "${_fossl_work_dir}/CMakeLists.txt"
"cmake_minimum_required(VERSION 3.18)\n\
project(findopensslfeatures LANGUAGES C)\n\
set(CMAKE_C_STANDARD 99)\n\
find_package(OpenSSL REQUIRED)\n\
add_executable(findopensslfeatures findopensslfeatures.c)\n\
target_include_directories(findopensslfeatures PRIVATE ${OPENSSL_INCLUDE_DIR})\n\
target_link_libraries(findopensslfeatures PRIVATE OpenSSL::Crypto)\n\
if (OpenSSL::applink)\n\
  target_link_libraries(findopensslfeatures PRIVATE OpenSSL::applink)\n\
endif(OpenSSL::applink)\n"
)

set(MKF ${MKF} "-DCMAKE_BUILD_TYPE=Release" "-DOPENSSL_ROOT_DIR=${OPENSSL_INCLUDE_DIR}/..")

if(CMAKE_PREFIX_PATH)
  set(MKF ${MKF} "-DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}")
endif(CMAKE_PREFIX_PATH)

if(CMAKE_TOOLCHAIN_FILE)
  set(MKF ${MKF} "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}")
endif(CMAKE_TOOLCHAIN_FILE)

if(CMAKE_GENERATOR_PLATFORM)
  set(MKF ${MKF} "-A" "${CMAKE_GENERATOR_PLATFORM}")
endif(CMAKE_GENERATOR_PLATFORM)

if(CMAKE_GENERATOR_TOOLSET)
  set(MKF ${MKF} "-T" "${CMAKE_GENERATOR_TOOLSET}")
endif(CMAKE_GENERATOR_TOOLSET)

execute_process(
  COMMAND "${CMAKE_COMMAND}" "-Bbuild" ${MKF} "."
  WORKING_DIRECTORY "${_fossl_work_dir}"
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error
  RESULT_VARIABLE result
  COMMAND_ECHO STDOUT
  ECHO_OUTPUT_VARIABLE
  ECHO_ERROR_VARIABLE
)

if (NOT ${result} EQUAL 0)
  message(FATAL_ERROR "Error configuring findopensslfeatures")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" "--build" "build" --config "Release"
  WORKING_DIRECTORY "${_fossl_work_dir}"
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error
  RESULT_VARIABLE result
  COMMAND_ECHO STDOUT
  ECHO_OUTPUT_VARIABLE
  ECHO_ERROR_VARIABLE
)

if (NOT ${result} EQUAL 0)
  message(FATAL_ERROR "Error building findopensslfeatures")
endif()

set(OPENSSL_SUPPORTED_FEATURES "")
if(WIN32 AND NOT MINGW)
  set(FOF "build/Release/findopensslfeatures")
else(WIN32 AND NOT MINGW)
  set(FOF "build/findopensslfeatures")
endif(WIN32 AND NOT MINGW)

foreach(feature "hashes" "ciphers" "curves" "publickey" "providers")
  execute_process(
    COMMAND "${FOF}" "${feature}"
    WORKING_DIRECTORY "${_fossl_work_dir}"
    OUTPUT_VARIABLE feature_val
    ERROR_VARIABLE error
    RESULT_VARIABLE result
  )

  if(NOT ${result} EQUAL 0)
    message(FATAL_ERROR "Error getting supported OpenSSL ${feature}: ${result}\n${error}")
  endif()

  string(TOUPPER ${feature} feature_up)
  string(TOUPPER ${feature_val} feature_val)
  string(REPLACE "\n" ";" feature_val ${feature_val})
  set(OPENSSL_SUPPORTED_${feature_up} ${feature_val})
  list(LENGTH OPENSSL_SUPPORTED_${feature_up} ${feature}_len)
  list(APPEND OPENSSL_SUPPORTED_FEATURES ${OPENSSL_SUPPORTED_${feature_up}})
endforeach()

message(STATUS "Fetched OpenSSL features: ${hashes_len} hashes, ${ciphers_len} ciphers, ${curves_len} curves, ${publickey_len} publickey, ${providers_len} providers.")

function(OpenSSLHasFeature FEATURE VARIABLE)
  string(TOUPPER ${FEATURE} _feature_up)
  set(${VARIABLE} FALSE PARENT_SCOPE)
  if (${_feature_up} IN_LIST OPENSSL_SUPPORTED_FEATURES)
      set(${VARIABLE} TRUE PARENT_SCOPE)
  endif()
endfunction(OpenSSLHasFeature)
