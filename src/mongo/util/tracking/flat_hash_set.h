// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"
#include "mongo/util/tracking/allocator.h"
#include "mongo/util/tracking/context.h"

#include <scoped_allocator>

#include <absl/container/flat_hash_set.h>
#include <absl/container/internal/hash_function_defaults.h>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace tracking {


template <class Key,
          class Hash = absl::container_internal::hash_default_hash<Key>,
          class Eq = absl::container_internal::hash_default_eq<Key>>
using flat_hash_set =
    absl::flat_hash_set<Key, Hash, Eq, std::scoped_allocator_adaptor<Allocator<Key>>>;

template <class Key>
flat_hash_set<Key> make_flat_hash_set(Context& Context) {
    return flat_hash_set<Key>(Context.makeAllocator<Key>());
}

}  // namespace tracking
}  // namespace mongo
