// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

// This file may be include multiple times, do not add #pragma once here

#if defined(_MSC_VER)
#  pragma warning(push)
#  if ((defined(__cplusplus) && __cplusplus >= 201704L) || \
       (defined(_MSVC_LANG) && _MSVC_LANG >= 201704L))
#    pragma warning(disable : 4996)
#    pragma warning(disable : 4309)
#    if _MSC_VER >= 1922
#      pragma warning(disable : 5054)
#    endif
#  endif

#  if _MSC_VER < 1910
#    pragma warning(disable : 4800)
#  endif
#  pragma warning(disable : 4244)
#  pragma warning(disable : 4251)
#  pragma warning(disable : 4267)
#  pragma warning(disable : 4668)
#  pragma warning(disable : 4946)
#  pragma warning(disable : 6001)
#  pragma warning(disable : 6244)
#  pragma warning(disable : 6246)
#endif

#if defined(__GNUC__) && !defined(__clang__) && !defined(__apple_build_version__)
#  if (__GNUC__ * 100 + __GNUC_MINOR__ * 10) >= 460
#    pragma GCC diagnostic push
#  endif
#  pragma GCC diagnostic ignored "-Wunused-parameter"
#  pragma GCC diagnostic ignored "-Wtype-limits"
#  pragma GCC diagnostic ignored "-Wsign-compare"
#  pragma GCC diagnostic ignored "-Wsign-conversion"
#  pragma GCC diagnostic ignored "-Wshadow"
#  pragma GCC diagnostic ignored "-Wuninitialized"
#  pragma GCC diagnostic ignored "-Wconversion"
#  if (__GNUC__ * 100 + __GNUC_MINOR__) >= 409
#    pragma GCC diagnostic ignored "-Wfloat-conversion"
#  endif
#  if (__GNUC__ * 100 + __GNUC_MINOR__) >= 501
#    pragma GCC diagnostic ignored "-Wsuggest-override"
#  endif
#elif defined(__clang__) || defined(__apple_build_version__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wunused-parameter"
#  pragma clang diagnostic ignored "-Wtype-limits"
#  pragma clang diagnostic ignored "-Wshadow-field"
#  pragma clang diagnostic ignored "-Wsign-compare"
#  pragma clang diagnostic ignored "-Wsign-conversion"
#  pragma clang diagnostic ignored "-Wshadow"
#  pragma clang diagnostic ignored "-Wuninitialized"
#  pragma clang diagnostic ignored "-Wconversion"
#  if ((__clang_major__ * 100) + __clang_minor__) >= 305
#    pragma clang diagnostic ignored "-Wfloat-conversion"
#  endif
#  if ((__clang_major__ * 100) + __clang_minor__) >= 306
#    pragma clang diagnostic ignored "-Winconsistent-missing-override"
#  endif
#  if ((__clang_major__ * 100) + __clang_minor__) >= 1100
#    pragma clang diagnostic ignored "-Wsuggest-override"
#  endif
#endif
