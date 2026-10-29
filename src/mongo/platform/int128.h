// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string>

#include <absl/numeric/int128.h>

namespace [[MONGO_MOD_PUBLIC]] absl {

std::string toString(const uint128& v);
std::string toString(const int128& v);

}  // namespace absl

namespace [[MONGO_MOD_PUBLIC]] mongo {

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
using make_unsigned_t [[MONGO_MOD_PUBLIC]] = typename make_unsigned<T>::type;

template <typename T>
using make_signed_t [[MONGO_MOD_PUBLIC]] = typename make_signed<T>::type;
}  // namespace mongo
