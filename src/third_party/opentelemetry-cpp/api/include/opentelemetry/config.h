// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <type_traits>  // IWYU pragma: keep

#if defined(OPENTELEMETRY_ABI_VERSION_NO) && OPENTELEMETRY_ABI_VERSION_NO >= 2
#  error \
      "opentelemetry/config.h is removed in ABI version 2 and later. Please use opentelemetry/version.h instead."
#else
#  if defined(__clang__) || defined(__GNUC__)
#    pragma GCC warning \
        "opentelemetry/config.h is deprecated. Please use opentelemetry/version.h instead."
#  elif defined(_MSC_VER)
#    pragma message( \
        "[WARNING]: opentelemetry/config.h is deprecated. Please use opentelemetry/version.h instead.")
#  endif
#endif
