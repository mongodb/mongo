// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <cstddef>
#include <vector>

namespace [[MONGO_MOD_PUBLIC]] mongo {

// Represents one modification from a source to a target. Specifies a change of 'targetSize' bytes
// starting at 'targetOffset', with the replacement data being 'sourceSize' bytes from
// 'sourceOffset'. The base addresses for these offsets are handled externally and not captured
// here.
struct DamageEvent {
    DamageEvent() = default;

    DamageEvent(size_t srcOffset, size_t srcSize, size_t tgtOffset, size_t tgtSize)
        : sourceOffset(srcOffset),
          sourceSize(srcSize),
          targetOffset(tgtOffset),
          targetSize(tgtSize) {}

    size_t sourceOffset;  // Offset from some base address to copy data from
    size_t sourceSize;    // Size of source data to copy. If 0, delete target data at targetOffset
    size_t targetOffset;  // Offset from some base address to apply the damage
    size_t targetSize;  // Size of target data to replace. If 0, insert source data at targetOffset
};

using DamageVector = std::vector<DamageEvent>;

}  // namespace mongo
