// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"
#include "mongo/util/tracking/allocator.h"
#include "mongo/util/tracking/context.h"

#include <scoped_allocator>
#include <string>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace tracking {

using string = std::basic_string<char, std::char_traits<char>, Allocator<char>>;

template <class... Args>
string make_string(Context& Context, Args... args) {
    return string(args..., Context.makeAllocator<char>());
}

}  // namespace tracking
}  // namespace mongo
