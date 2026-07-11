// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <cstddef>

namespace mongo {

class BufBuilder;

template <std::size_t SZ>
class StackBufBuilderBase;

inline constexpr std::size_t StackSizeDefault = 512;

using StackBufBuilder [[MONGO_MOD_PUBLIC]] = StackBufBuilderBase<StackSizeDefault>;

template <typename Allocator>
class StringBuilderImpl;

using StringBuilder [[MONGO_MOD_PUBLIC]] = StringBuilderImpl<BufBuilder>;
using StackStringBuilder [[MONGO_MOD_PUBLIC]] = StringBuilderImpl<StackBufBuilder>;

}  // namespace mongo
