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

#include "mongo/util/tick_source.h"
#include "mongo/util/time_support.h"

namespace mongo {

/**
 * Mock tick source that can be parameterized on a duration type.
 *
 * Its internal tick count will be tracked in the unit of the duration type parameter. For example,
 * for TickSourceMock<Milliseconds>, 1 tick = 1 millisecond. It gives a fixed tick count until the
 * advance method is called.
 */
template <typename D = Milliseconds>
class TickSourceMock final : public TickSource {
public:
    TickSource::Tick getTicks() override {
        return _currentTicks;
    };

    TickSource::Tick getTicksPerSecond() override {
        static_assert(D::period::num == 1,
                      "Cannot measure ticks per second for duration types larger than 1 second.");
        return D::period::den;
    };

    /**
     * Advance the ticks by the given amount of milliseconds.
     */
    void advance(const D& duration) {
        _currentTicks += duration.count();
    }

    /**
     * Resets the tick count to the given value.
     */
    void reset(TickSource::Tick tick) {
        _currentTicks = std::move(tick);
    }

private:
    TickSource::Tick _currentTicks = 0;
};
}  // namespace mongo
