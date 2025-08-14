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

#include "mongo/util/modules.h"

#include <string>

#include <absl/numeric/int128.h>

namespace MONGO_MOD_PUB absl {

std::string toString(const uint128& v);
std::string toString(const int128& v);

}  // namespace MONGO_MOD_PUB absl

namespace MONGO_MOD_PUB mongo {

using uint128_t = absl::uint128;
using int128_t = absl::int128;

template <typename T>
struct make_unsigned : public std::make_unsigned<T> {};

template <>
struct make_unsigned<int128_t> {
    using type = uint128_t;
};

template <typename T>
struct make_signed : public std::make_signed<T> {};

template <>
struct make_signed<uint128_t> {
    using type = int128_t;
};

template <typename T>
using make_unsigned_t MONGO_MOD_PUB = typename make_unsigned<T>::type;

template <typename T>
using make_signed_t MONGO_MOD_PUB = typename make_signed<T>::type;
}  // namespace MONGO_MOD_PUB mongo
