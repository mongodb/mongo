// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

// This file depends on internal implementation details in abseil. In a library upgrade you may have
// to re-write this file significantly.
#include "mongo/util/modules.h"

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

namespace [[MONGO_MOD_PUBLIC]] mongo {
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
