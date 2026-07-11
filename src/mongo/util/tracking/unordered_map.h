// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/stdx/unordered_map.h"
#include "mongo/util/modules.h"
#include "mongo/util/tracking/allocator.h"
#include "mongo/util/tracking/context.h"

#include <scoped_allocator>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace tracking {

template <class Key,
          class Value,
          class Hasher = DefaultHasher<Key>,
          class KeyEqual = std::equal_to<Key>>
using unordered_map =
    stdx::unordered_map<Key,
                        Value,
                        Hasher,
                        KeyEqual,
                        std::scoped_allocator_adaptor<Allocator<std::pair<const Key, Value>>>>;

template <class Key,
          class Value,
          class Hasher = DefaultHasher<Key>,
          class KeyEqual = std::equal_to<Key>>
unordered_map<Key, Value, Hasher> make_unordered_map(Context& Context) {
    return unordered_map<Key, Value, Hasher, KeyEqual>(Context.makeAllocator<Value>());
}

}  // namespace tracking
}  // namespace mongo
