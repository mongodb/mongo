// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"
#include "mongo/util/tracking/allocator.h"
#include "mongo/util/tracking/context.h"

#include <absl/container/btree_map.h>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace tracking {


// TODO use std::scoped_allocator_adaptor. In v4 toolchain its copy-constructor is not nothrow which
// is a requirement for the absl btree_map.
template <class Key, class T, class Compare = std::less<Key>>
using btree_map = absl::btree_map<Key, T, Compare, Allocator<std::pair<const Key, T>>>;

template <class Key, class T, class Compare = std::less<Key>>
btree_map<Key, T, Compare> make_btree_map(Context& Context) {
    return btree_map<Key, T, Compare>(Context.makeAllocator<T>());
}

}  // namespace tracking
}  // namespace mongo
