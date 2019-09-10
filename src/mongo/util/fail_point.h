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

#include <functional>

#include "mongo/base/status_with.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

/**
 * A simple thread-safe fail point implementation that can be activated and
 * deactivated, as well as embed temporary data into it.
 *
 * The fail point has a static instance, which is represented by a FailPoint
 * object, and dynamic instance, represented by FailPoint::Scoped handles.
 *
 * Sample use:
 * // Declared somewhere:
 * FailPoint makeBadThingsHappen;
 *
 * // Somewhere in the code
 * return false || MONGO_unlikely(makeBadThingsHappen.shouldFail());
 *
 * or
 *
 * // Somewhere in the code
 * makeBadThingsHappen.execute([&](const BSONObj& data) {
 *     // Do something
 * });
 *
 * // Another way to do it, where lambda isn't suitable, e.g. to cause an early return
 * // of the enclosing function.
 * if (auto sfp = makeBadThingsHappen.scoped(); MONGO_unlikely(sfp.isActive())) {
 *     const BSONObj& data = sfp.getData();
 *     // Do something, including break, continue, return, etc...
 * }
 *
 * Invariants:
 *
 * 1. Always refer to _fpInfo first to check if failPoint is active or not before
 *    entering fail point or modifying fail point.
 * 2. Client visible fail point states are read-only when active.
 */
class FailPoint {
private:
    enum RetCode { fastOff = 0, slowOff, slowOn, userIgnored };

public:
    using ValType = unsigned;
    enum Mode { off, alwaysOn, random, nTimes, skip };

    struct ModeOptions {
        Mode mode;
        ValType val;
        BSONObj extra;
    };

    /**
     * Helper class for making sure that FailPoint#shouldFailCloseBlock is called when
     * FailPoint#shouldFailOpenBlock was called.
     *
     * Users don't create these. They are only used within the execute and executeIf
     * functions and returned by the scoped() and scopedIf() functions.
     */
    class Scoped {
    public:
        Scoped(FailPoint* failPoint, RetCode ret)
            : _failPoint(failPoint),
              _active(ret == FailPoint::slowOn),
              _holdsRef(ret != FailPoint::fastOff) {}

        ~Scoped() {
            if (_holdsRef) {
                _failPoint->shouldFailCloseBlock();
            }
        }

        Scoped(const Scoped&) = delete;
        Scoped& operator=(const Scoped&) = delete;

        /**
         * @return true if fail point is on.
         * Calls to isActive should be placed inside MONGO_unlikely for performance.
         */
        bool isActive() {
            return _active;
        }

        /**
         * @return the data stored in the fail point. #isActive must be true
         *     before you can call this.
         */
        const BSONObj& getData() const {
            // Assert when attempting to get data without holding a ref.
            fassert(16445, _holdsRef);
            return _failPoint->getData();
        }

    private:
        FailPoint* _failPoint;
        bool _active;
        bool _holdsRef;
    };

    /**
     * Explicitly resets the seed used for the PRNG in this thread.  If not called on a thread,
     * an instance of SecureRandom is used to seed the PRNG.
     */
    static void setThreadPRNGSeed(int32_t seed);

    /**
     * Parses the {Mode, ValType, BSONObj} from the BSON.
     */
    static StatusWith<ModeOptions> parseBSON(const BSONObj& obj);

    FailPoint();

    FailPoint(const FailPoint&) = delete;
    FailPoint& operator=(const FailPoint&) = delete;

    /**
     * Note: This is not side-effect free - it can change the state to OFF after calling.
     * Note: see `executeIf` for information on `pred`.
     *
     * Calls to shouldFail should be placed inside MONGO_unlikely for performance.
     *
     * @return true if fail point is active.
     */
    template <typename Pred>
    bool shouldFail(Pred&& pred) {
        RetCode ret = shouldFailOpenBlock(std::forward<Pred>(pred));

        if (MONGO_likely(ret == fastOff)) {
            return false;
        }

        shouldFailCloseBlock();
        return ret == slowOn;
    }

    bool shouldFail() {
        return shouldFail(nullptr);
    }

    /**
     * Changes the settings of this fail point. This will turn off the fail point
     * and waits for all dynamic instances referencing this fail point to go away before
     * actually modifying the settings.
     *
     * @param mode the new mode for this fail point.
     * @param val the value that can have different usage depending on the mode:
     *
     *     - off, alwaysOn: ignored
     *     - random: static_cast<int32_t>(std::numeric_limits<int32_t>::max() * p), where
     *           where p is the probability that any given evaluation of the failpoint should
     *           activate.
     *     - nTimes: the number of times this fail point will be active when
     *         #shouldFail or #shouldFailOpenBlock is called.
     *     - skip: the number of times this failpoint will be inactive when
     *         #shouldFail or #shouldFailOpenBlock is called. After this number is reached, the
     *         failpoint will always be active.
     *
     * @param extra arbitrary BSON object that can be stored to this fail point
     *     that can be referenced afterwards with #getData. Defaults to an empty
     *     document.
     */
    void setMode(Mode mode, ValType val = 0, BSONObj extra = {});
    void setMode(ModeOptions opt) {
        setMode(std::move(opt.mode), std::move(opt.val), std::move(opt.extra));
    }

    /**
     * @returns a BSON object showing the current mode and data stored.
     */
    BSONObj toBSON() const;

    /**
     * Create a Scoped from this FailPoint.
     * Use the Scoped object to access failpoint data.
     */
    Scoped scoped() {
        return scopedIf(nullptr);
    }
    /**
     * Create a Scoped from this FailPoint, only active when `pred(payload)` is true.
     * See `executeIf`. Use the Scoped object to access failpoint data.
     */
    template <typename Pred>
    Scoped scopedIf(Pred&& pred) {
        return Scoped(this, shouldFailOpenBlock(std::forward<Pred>(pred)));
    }

    template <typename F>
    void execute(F&& f) {
        return executeIf(f, nullptr);
    }

    /**
     * The predicate `pred` should behave like a `bool pred(const BSONObj& payload)`.
     * If `pred(payload)`, then `f(payload)` is executed. Otherwise, `f` is not
     * executed and this FailPoint's mode is not altered (e.g. `nTimes` isn't consumed).
     */
    template <typename F, typename Pred>
    void executeIf(F&& f, Pred&& pred) {
        auto sfp = scopedIf(std::forward<Pred>(pred));
        if (MONGO_unlikely(sfp.isActive())) {
            std::forward<F>(f)(sfp.getData());
        }
    }

    void pauseWhileSet() {
        while (MONGO_unlikely(shouldFail())) {
            sleepmillis(100);
        }
    }

    void pauseWhileSet(OperationContext* opCtx) {
        while (MONGO_unlikely(shouldFail())) {
            opCtx->sleepFor(Milliseconds(100));
        }
    }

private:
    void enable();
    void disable();

    /**
     * Checks whether fail point is active and increments the reference counter without
     * decrementing it. Must call shouldFailCloseBlock afterwards when the return value
     * is not fastOff. Otherwise, this will remain read-only forever.
     *
     * Note: see `executeIf` for information on `pred`.
     *
     * @return slowOn if its active and needs to be closed
     *         userIgnored if its active and needs to be closed, but shouldn't be acted on
     *         slowOff if its disabled and needs to be closed
     *         fastOff if its disabled and doesn't need to be closed
     */
    template <typename Pred>
    RetCode shouldFailOpenBlock(Pred&& pred) {
        if (MONGO_likely((_fpInfo.loadRelaxed() & kActiveBit) == 0)) {
            return fastOff;
        }

        return slowShouldFailOpenBlock(std::forward<Pred>(pred));
    }

    RetCode shouldFailOpenBlock() {
        return shouldFailOpenBlock(nullptr);
    }

    /**
     * Decrements the reference counter.
     * @see #shouldFailOpenBlock
     */
    void shouldFailCloseBlock();

    /**
     * slow path for #shouldFailOpenBlock
     *
     * If a callable is passed, and returns false, this will return userIgnored and avoid altering
     * the mode in any way.  The argument is the fail point payload.
     */
    RetCode slowShouldFailOpenBlock(std::function<bool(const BSONObj&)> cb) noexcept;

    /**
     * @return the stored BSONObj in this fail point. Note that this cannot be safely
     *      read if this fail point is off.
     */
    const BSONObj& getData() const;

    static const ValType kActiveBit = 1 << 31;

    // Bit layout:
    // 31: tells whether this fail point is active.
    // 0~30: unsigned ref counter for active dynamic instances.
    AtomicWord<std::uint32_t> _fpInfo{0};

    // Invariant: These should be read only if kActiveBit of _fpInfo is set.
    Mode _mode{off};
    AtomicWord<int> _timesOrPeriod{0};
    BSONObj _data;

    // protects _mode, _timesOrPeriod, _data
    mutable stdx::mutex _modMutex;
};

}  // namespace mongo
