// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/stdx/trusted_hasher.h"
#include "mongo/util/modules.h"

#include <cstddef>

#include <absl/container/node_hash_map.h>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace stdx {

template <class Key, class Value, class Hasher = DefaultHasher<Key>, typename... Args>
using unordered_map = absl::node_hash_map<Key, Value, EnsureTrustedHasher<Hasher, Key>, Args...>;

/**
 * Removes the elements from `c` for which `pred(elem)` is true.
 * Returns the count of elements erased.
 * See https://en.cppreference.com/w/cpp/container/unordered_map/erase_if
 * Workaround for the `void erase(iterator)` of `absl::node_hash_map`.
 */
template <typename Key, typename T, typename Hash, typename Eq, typename Alloc, typename Pred>
size_t erase_if(absl::node_hash_map<Key, T, Hash, Eq, Alloc>& c, Pred&& pred) {
    auto oldSize = c.size();
    for (auto i = c.begin(), last = c.end(); i != last;)
        if (pred(*i))
            c.erase(i++);
        else
            ++i;
    return oldSize - c.size();
}

}  // namespace stdx
}  // namespace mongo
