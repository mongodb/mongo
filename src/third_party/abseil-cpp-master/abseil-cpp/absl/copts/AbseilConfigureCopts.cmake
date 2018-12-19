# See absl/copts/copts.py and absl/copts/generate_copts.py
include(GENERATED_AbseilCopts)

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
  set(ABSL_DEFAULT_COPTS "${GCC_FLAGS}")
  set(ABSL_TEST_COPTS "${GCC_FLAGS};${GCC_TEST_FLAGS}")
  set(ABSL_EXCEPTIONS_FLAG "${GCC_EXCEPTIONS_FLAGS}")
elseif("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
  # MATCHES so we get both Clang and AppleClang
  set(ABSL_DEFAULT_COPTS "${LLVM_FLAGS}")
  set(ABSL_TEST_COPTS "${LLVM_FLAGS};${LLVM_TEST_FLAGS}")
  set(ABSL_EXCEPTIONS_FLAG "${LLVM_EXCEPTIONS_FLAGS}")
elseif("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
  set(ABSL_DEFAULT_COPTS "${MSVC_FLAGS}")
  set(ABSL_TEST_COPTS "${MSVC_FLAGS};${MSVC_TEST_FLAGS}")
  set(ABSL_EXCEPTIONS_FLAG "${MSVC_EXCEPTIONS_FLAGS}")
else()
  message(WARNING "Unknown compiler: ${CMAKE_CXX_COMPILER}.  Building with no default flags")
  set(ABSL_DEFAULT_COPTS "")
  set(ABSL_TEST_COPTS "")
  set(ABSL_EXCEPTIONS_FLAG "")
endif()

# This flag is used internally for Bazel builds and is kept here for consistency
set(ABSL_EXCEPTIONS_FLAG_LINKOPTS "")

if("${CMAKE_CXX_STANDARD}" EQUAL 98)
  message(FATAL_ERROR "Abseil requires at least C++11")
elseif(NOT "${CMAKE_CXX_STANDARD}")
  message(STATUS "No CMAKE_CXX_STANDARD set, assuming 11")
  set(ABSL_CXX_STANDARD 11)
else()
  set(ABSL_CXX_STANDARD "${CMAKE_CXX_STANDARD}")
endif()