/*
 *    Copyright (C) 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include "mongo/base/disallow_copying.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/concurrency/mutex.h"

namespace mongo {
    /**
     * A simple thread-safe fail point implementation that can be activated and
     * deactivated, as well as embed temporary data into it.
     *
     * The fail point has a static instance, which is represented by a FailPoint
     * object, and dynamic instance, which are all the threads in between
     * shouldFailOpenBlock and shouldFailCloseBlock.
     *
     * Sample use:
     * // Declared somewhere:
     * FailPoint makeBadThingsHappen;
     *
     * // Somewhere in the code
     * return false || MONGO_FAIL_POINT(makeBadThingsHappen);
     *
     * or
     *
     * // Somewhere in the code
     * MONGO_FAIL_POINT_BLOCK(makeBadThingsHappen, blockMakeBadThingsHappen) {
     *     const BSONObj& data = blockMakeBadThingsHappen.getData();
     *     // Do something
     * }
     *
     * Invariants:
     *
     * 1. Always refer to _fpInfo first to check if failPoint is active or not before
     *    entering fail point or modifying fail point.
     * 2. Client visible fail point states are read-only when active.
     */
    class FailPoint {
        MONGO_DISALLOW_COPYING(FailPoint);

    public:
        typedef AtomicUInt32::WordType ValType;
        enum Mode { off, alwaysOn, random, nTimes, numModes };
        enum RetCode { fastOff = 0, slowOff, slowOn };

        FailPoint();

        /**
         * Note: This is not side-effect free - it can change the state to OFF after calling.
         *
         * @return true if fail point is active.
         */
        inline bool shouldFail() {
            RetCode ret = shouldFailOpenBlock();

            if (MONGO_likely(ret == fastOff)) {
                return false;
            }

            shouldFailCloseBlock();
            return ret == slowOn;
        }

        /**
         * Checks whether fail point is active and increments the reference counter without
         * decrementing it. Must call shouldFailCloseBlock afterwards when the return value
         * is not fastOff. Otherwise, this will remain read-only forever.
         *
         * @return slowOn if fail point is active.
         */
        inline RetCode shouldFailOpenBlock() {
            if (MONGO_likely((_fpInfo.loadRelaxed() & ACTIVE_BIT) == 0)) {
                return fastOff;
            }

            return slowShouldFailOpenBlock();
        }

        /**
         * Decrements the reference counter.
         * @see #shouldFailOpenBlock
         */
        void shouldFailCloseBlock();

        /**
         * Changes the settings of this fail point. This will turn off the fail point
         * and waits for all dynamic instances referencing this fail point to go away before
         * actually modifying the settings.
         *
         * @param mode the new mode for this fail point.
         * @param val the value that can have different usage depending on the mode:
         *
         *     - off, alwaysOn: ignored
         *     - random:
         *     - nTimes: the number of times this fail point will be active when
         *         #shouldFail or #shouldFailOpenBlock is called.
         *
         * @param extra arbitrary BSON object that can be stored to this fail point
         *     that can be referenced afterwards with #getData. Defaults to an empty
         *     document.
         */
        void setMode(Mode mode, ValType val = 0, const BSONObj& extra = BSONObj());

        /**
         * @returns a BSON object showing the current mode and data stored.
         */
        BSONObj toBSON() const;

    private:
        static const ValType ACTIVE_BIT = 1 << 31;
        static const ValType REF_COUNTER_MASK = ~ACTIVE_BIT;

        // Bit layout:
        // 31: tells whether this fail point is active.
        // 0~30: unsigned ref counter for active dynamic instances.
        AtomicUInt32 _fpInfo;

        // Invariant: These should be read only if ACTIVE_BIT of _fpInfo is set.
        Mode _mode;
        AtomicInt32 _timesOrPeriod;
        BSONObj _data;

        // protects _mode, _timesOrPeriod, _data
        mutable mutex _modMutex;

        /**
         * Enables this fail point.
         */
        void enableFailPoint();

        /**
         * Disables this fail point.
         */
        void disableFailPoint();

        /**
         * slow path for #shouldFailOpenBlock
         */
        RetCode slowShouldFailOpenBlock();

        /**
         * @return the stored BSONObj in this fail point. Note that this cannot be safely
         *      read if this fail point is off.
         */
        const BSONObj& getData() const;

        friend class ScopedFailPoint;
    };

    /**
     * Helper class for making sure that FailPoint#shouldFailCloseBlock is called when
     * FailPoint#shouldFailOpenBlock was called. This should only be used within the
     * MONGO_FAIL_POINT_BLOCK macro.
     */
    class ScopedFailPoint {
        MONGO_DISALLOW_COPYING(ScopedFailPoint);

    public:
        ScopedFailPoint(FailPoint* failPoint);
        ~ScopedFailPoint();

        /**
         * @return true if fail point is on. This will be true at most once.
         */
        inline bool isActive() {
            if (_once) {
                return false;
            }

            _once = true;

            FailPoint::RetCode ret = _failPoint->shouldFailOpenBlock();
            _shouldClose = ret != FailPoint::fastOff;
            return ret == FailPoint::slowOn;
        }

        /**
         * @return the data stored in the fail point. #isActive must be true
         *     before you can call this.
         */
        const BSONObj& getData() const;

    private:
        FailPoint* _failPoint;
        bool _once;
        bool _shouldClose;
    };

    #define MONGO_FAIL_POINT(symbol) MONGO_unlikely(symbol.shouldFail())

    /**
     * Macro for creating a fail point with block context. Also use this when
     * you want to access the data stored in the fail point.
     */
    #define MONGO_FAIL_POINT_BLOCK(symbol, blockSymbol) \
        for (mongo::ScopedFailPoint blockSymbol(&symbol); \
            MONGO_unlikely(blockSymbol.isActive()); )
}
