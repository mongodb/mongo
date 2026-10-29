// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"
#include "mongo/util/tracking/allocator.h"
#include "mongo/util/tracking/context.h"

#include <scoped_allocator>

#include <absl/container/inlined_vector.h>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace tracking {

template <class T, std::size_t N>
using inlined_vector = absl::InlinedVector<T, N, std::scoped_allocator_adaptor<Allocator<T>>>;

template <class T, std::size_t N>
inlined_vector<T, N> make_inlined_vector(Context& Context) {
    return inlined_vector<T, N>(Context.makeAllocator<T>());
}

}  // namespace tracking
}  // namespace mongo
