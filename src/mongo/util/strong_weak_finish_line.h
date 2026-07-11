// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"

#include <cstdint>

[[MONGO_MOD_PUBLIC]];

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
    constexpr static inline uint64_t kHighBit = 1ull << 63;

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
        if ((_state.load() & kHighBit) || (_state.fetchAndBitOr(kHighBit) & kHighBit)) {
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
        return _state.subtractAndFetch(1) == 0;
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
    Atomic<uint64_t> _state;
};

}  // namespace mongo
