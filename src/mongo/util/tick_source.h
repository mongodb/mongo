// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <algorithm>
#include <cstdint>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * Interface for objects generating ticks that roughly represents the passage of time.
 */
class [[MONGO_MOD_OPEN]] TickSource {
public:
    using Tick = int64_t;

    virtual ~TickSource() = default;

    /**
     * Returns the current tick count from this source.
     */
    virtual Tick getTicks() = 0;

    /**
     * Returns the conversion ratio from ticks to seconds.
     */
    virtual Tick getTicksPerSecond() = 0;

    /**
     * Convert the given tick count into a duration, specified by the type parameter.
     *
     * e.g. tickSource->ticksTo<Milliseconds>(ticks);
     */
    template <typename D>
    D ticksTo(Tick ticks) {
        // The number of ticks per 1 duration unit.
        double ticksPerD =
            static_cast<double>(getTicksPerSecond()) * D::period::num / D::period::den;
        return D(static_cast<int64_t>(ticks / ticksPerD));
    }
};
}  // namespace mongo
