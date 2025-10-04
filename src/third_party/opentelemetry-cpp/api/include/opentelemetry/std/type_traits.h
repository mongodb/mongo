// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

// IWYU pragma: private, include "opentelemetry/nostd/type_traits.h"

#include <type_traits>

#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
// Standard Type aliases in nostd namespace
namespace nostd
{

// nostd::enable_if_t<...>
template <bool B, class T = void>
using enable_if_t = typename std::enable_if<B, T>::type;

}  // namespace nostd
OPENTELEMETRY_END_NAMESPACE
