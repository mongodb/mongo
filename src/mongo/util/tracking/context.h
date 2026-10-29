// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/tracking/allocator.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace tracking {

/**
 * A Context is a factory style class that constructs Allocator objects under a
 * single instance of AllocatorStats and provides access to these stats.
 */
class Context {
public:
    Context() = default;
    ~Context() = default;

    template <class T>
    Allocator<T> makeAllocator() {
        return Allocator<T>(_stats);
    }

    AllocatorStats& stats() {
        return _stats;
    }

    uint64_t allocated() const {
        return _stats.allocated();
    }

private:
    AllocatorStats _stats{ProcessInfo::getNumLogicalCores() * 2};
};

}  // namespace tracking
}  // namespace mongo
