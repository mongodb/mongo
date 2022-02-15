/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <algorithm>
#include <cstdint>

namespace mongo {

/**
 * Interface for objects generating ticks that roughly represents the passage of time.
 */
class TickSource {
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

    /**
     * Measures the length of the span from the start tick to the end tick and returns the result
     * using duration type D.
     * If the start tick is after (greater than) the end tick, returns a duration equivalent to 0
     * ticks.
     *
     * e.g. tickSource->spanTo<Milliseconds>(start, end);
     */
    template <typename D>
    D spanTo(Tick start, Tick end) {
        return ticksTo<D>(std::max((end - start), Tick{0}));
    }
};
}  // namespace mongo
