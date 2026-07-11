// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"
#include "mongo/util/tracking/allocator.h"
#include "mongo/util/tracking/context.h"

#include <map>
#include <scoped_allocator>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace tracking {

template <class Key, class T, class Compare = std::less<Key>>
using map =
    std::map<Key, T, Compare, std::scoped_allocator_adaptor<Allocator<std::pair<const Key, T>>>>;

template <class Key, class T, class Compare = std::less<Key>>
map<Key, T, Compare> make_map(Context& Context) {
    return map<Key, T, Compare>(Context.makeAllocator<T>());
}

}  // namespace tracking
}  // namespace mongo
