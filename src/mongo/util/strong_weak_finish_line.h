/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <atomic>
#include <utility>

namespace mongo {

/**
 * A finish line that coordinates the winer of a race amongst a series of participants.
 *
 * The winner of the race is the first to arriveStrongly or the last to arriveWeakly (think the last
 * to get disqualified).
 *
 * Every caller of arriveStrong/Weak can depend on the fact that only one caller will be returned a
 * true value.  That means that consumers of this latch may use it to coordinate access to mutable
 * shared state as long as they only mutate that state when receiving a true response.
 */
class StrongWeakFinishLine {
    constexpr static uint64_t kHighBit = 1ull << 63;

public:
    explicit StrongWeakFinishLine(uint64_t n) : _state(n) {}

    /**
     * Returns true if this is the first call to arriveStrongly
     *
     * If the return value is true, a participant will know that it won the race.  Note that there
     * may still be other participants executing however (though none of them will return true from
     * a call to arrive).
     */
    bool arriveStrongly() {
        if ((_state.load() & kHighBit) || (_state.fetch_or(kHighBit) & kHighBit)) {
            return false;
        }

        return true;
    }

    /**
     * Returns true if it is the last call to arriveWeakly
     *
     * If the return value is true, a participant will know that it is the last caller and that all
     * other participants also called arriveWeakly.
     */
    bool arriveWeakly() {
        return --_state == 0;
    }

    /**
     * Returns true if someone has arrivedStrongly, or all participants have arrived weakly.
     */
    bool isReady() const {
        auto value = _state.load();
        return (value & kHighBit) || (value == 0);
    }

private:
    // Theory of operation for State
    //
    // Initial state: num participants
    //
    // On strong: atomic.fetch_or(highBit)
    //   Concurrent calls race to set the high bit.
    //
    // On weak: atomic.fetch_sub(1)
    //   Decrements state by 1.  If this reduces state to 0, there was no strong arriver.
    std::atomic<uint64_t> _state;  // NOLINT
};

}  // namespace mongo
