// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"
#include "mongo/util/tracking/allocator.h"
#include "mongo/util/tracking/context.h"

#include <scoped_allocator>
#include <set>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace tracking {

template <class Key>
using set = std::set<Key, std::less<Key>, std::scoped_allocator_adaptor<Allocator<Key>>>;

template <class Key>
set<Key> make_set(Context& Context) {
    return set<Key>(Context.makeAllocator<Key>());
}

}  // namespace tracking
}  // namespace mongo
