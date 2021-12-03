/**
 *    Copyright (C) 2021-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/pipeline/window_function/window_function_first_last_n.h"
#include "mongo/db/pipeline/window_function/window_function_min_max.h"
#include "mongo/db/pipeline/window_function/window_function_top_bottom_n.h"

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
