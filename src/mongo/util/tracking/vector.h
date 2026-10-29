// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"
#include "mongo/util/tracking/allocator.h"
#include "mongo/util/tracking/context.h"

#include <scoped_allocator>
#include <vector>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace tracking {
template <class T>
using vector = std::vector<T, std::scoped_allocator_adaptor<Allocator<T>>>;

template <class T, class... Args>
vector<T> make_vector(Context& Context, Args... args) {
    return vector<T>(args..., Context.makeAllocator<T>());
}

}  // namespace tracking
}  // namespace mongo
