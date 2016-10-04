/**
 *    Copyright (C) 2014 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <memory>

#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/time_support.h"

namespace mongo {

class ClockSource;
class Date_t;
class MmapV1RecordHeader;

/**
 * Used to implement likelyInPhysicalMemory() for the MMAP v1 storage engine. Since
 * MMAP v1 holds exclusive collection-level locks, it should yield the locks during a
 * page fault. The RecordAccessTracker is used to guess at which records are in memory,
 * so that a yield can be requested unless we're sure that the record has been
 * recently accessed.
 */
class RecordAccessTracker {
    MONGO_DISALLOW_COPYING(RecordAccessTracker);

public:
    RecordAccessTracker(ClockSource* cs);

    enum Constants {
        SliceSize = 1024,
        MaxChain = 20,  // intentionally very low
        NumSlices = 10,
        RotateTimeSecs = 90,
        BigHashSize = 128
    };

    /**
     * Informs this record access tracker that 'record' has been accessed.
     */
    void markAccessed(const void* record);

    /**
     * @return whether or not 'record' has been marked as accessed recently. A return value
     * of true means that 'record' is likely in physical memory.
     *
     * Also has the side effect of marking 'record' as accessed.
     */
    bool checkAccessedAndMark(const void* record);

    /**
     * Clears out any history of record accesses.
     */
    void reset();

    //
    // For testing.
    //

    /**
     * The accessedRecently() implementation falls back to making a system call if it
     * appears that the record is not in physical memory. Use this method to disable
     * the fallback for testing.
     */
    void disableSystemBlockInMemCheck();

private:
    enum State { In, Out, Unk };

    struct Entry {
        size_t region;
        unsigned long long value;
    };

    /**
     * simple hash map for region -> status
     * this constitutes a single region of time
     * it does chaining, but very short chains
     */
    class Slice {
    public:
        Slice();

        void reset();

        State get(int regionHash, size_t region, short offset);

        /**
         * @return true if added, false if full
         */
        bool put(int regionHash, size_t region, short offset);

    private:
        Entry* _get(int start, size_t region, bool add);

        Entry _data[SliceSize];
    };

    /**
     * this contains many slices of times
     * the idea you put mem status in the current time slice
     * and then after a certain period of time, it rolls off so we check again
     */
    class Rolling {
    public:
        Rolling() = default;

        /**
         * After this call, we assume the page is in RAM.
         *
         * @param doHalf if this is a known good access, want to put in first half.
         *
         * @return whether we know the page is in RAM
         */
        bool access(size_t region, short offset, bool doHalf, ClockSource* cs);

        /**
         * Updates _lastRotate to the current time.
         */
        void updateLastRotate(ClockSource* cs);

    private:
        void _rotate(ClockSource* cs);

        int _curSlice = 0;
        Date_t _lastRotate;
        Slice _slices[NumSlices];

        SimpleMutex _lock;
    };

    // Should this record tracker fallback to making a system call?
    bool _blockSupported;
    ClockSource* _clock;

    // An array of Rolling instances for tracking record accesses.
    std::unique_ptr<Rolling[]> _rollingTable;
};

}  // namespace
