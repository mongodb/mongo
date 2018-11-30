/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

// This file depends on internal implementation details in abseil. In a library upgrade you may have
// to re-write this file significantly.
#include <absl/container/internal/hash_function_defaults.h>

/**
 * To be safe we let abseil hash the produced hash one more time to protect ourselves against bad
 * hash functions. If you know your hash function is good and can be trusted you get mark it as
 * trusted by specializing a template like this:
 *
 * namespace mongo {
 *     template <>
 *     struct IsTrustedHasher<YourHash, YourKey> : std::true_type {};
 * }
 */

namespace mongo {
template <typename Key>
using DefaultHasher = absl::container_internal::hash_default_hash<Key>;

template <typename Hasher, typename Key>
struct IsTrustedHasher : std::is_same<Hasher, DefaultHasher<Key>> {};

template <typename Hasher, typename Key>
struct HashImprover : private Hasher {
    HashImprover(const Hasher& hasher = Hasher()) : Hasher(hasher) {}
    std::size_t operator()(const Key& k) const {
        return absl::Hash<std::size_t>{}(Hasher::operator()(k));
    }
};

template <typename Hasher, typename Key>
using EnsureTrustedHasher =
    std::conditional_t<IsTrustedHasher<Hasher, Key>::value, Hasher, HashImprover<Hasher, Key>>;

}  // namespace mongo
