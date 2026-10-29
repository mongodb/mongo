// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"
#include "mongo/util/tracking/allocator.h"
#include "mongo/util/tracking/context.h"

#include <list>
#include <scoped_allocator>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace tracking {

template <class T>
using list = std::list<T, std::scoped_allocator_adaptor<Allocator<T>>>;

template <class T>
list<T> make_list(Context& Context) {
    return list<T>(Context.makeAllocator<T>());
}

}  // namespace tracking
}  // namespace mongo
