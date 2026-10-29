// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/window_function/window_function_first_last_n.h"
#include "mongo/db/pipeline/window_function/window_function_min_max.h"
#include "mongo/db/pipeline/window_function/window_function_top_bottom_n.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace window_function_n_traits {

// All variations of WindowFunctionTopBottomN fulfill this trait.
// needsSortBy<T>::value evaluates to true if T fulfills this trait.
template <typename WindowFunctionType>
struct needsSortBy : std::false_type {};
template <TopBottomSense sense, bool single>
struct needsSortBy<WindowFunctionTopBottomN<sense, single>> : std::true_type {};

// $firstN, $lastN, $minN, $maxN, and Variants of WindowFunctionTopBottomN fulfill this trait.
// isWindowFunctionN<T>::value evaluates to true if T fulfills this trait.
template <typename WindowFunctionType>
struct isWindowFunctionN : needsSortBy<WindowFunctionType> {};
// TODO SERVER-59327 fix these after making MinN and MaxN both templated like top and bottomN?
template <>
struct isWindowFunctionN<WindowFunctionFirstN> : std::true_type {};
template <>
struct isWindowFunctionN<WindowFunctionLastN> : std::true_type {};
template <>
struct isWindowFunctionN<WindowFunctionMinN> : std::true_type {};
template <>
struct isWindowFunctionN<WindowFunctionMaxN> : std::true_type {};
}  // namespace window_function_n_traits
}  // namespace mongo
