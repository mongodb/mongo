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

#include "mongo/platform/basic.h"

#include "mongo/db/storage/mmap_v1/record_access_tracker.h"

#include <cstring>

#include "mongo/base/init.h"
#include "mongo/config.h"
#include "mongo/db/storage/mmap_v1/record.h"
#include "mongo/platform/bits.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/concurrency/threadlocal.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/net/listen.h"
#include "mongo/util/processinfo.h"

namespace mongo {

namespace {

static bool blockSupported = false;

MONGO_INITIALIZER_WITH_PREREQUISITES(RecordBlockSupported, ("SystemInfo"))(InitializerContext* cx) {
    blockSupported = ProcessInfo::blockCheckSupported();
    return Status::OK();
}

int hash(size_t region) {
    return abs(((7 + (int)(region & 0xFFFF)) * (11 + (int)((region >> 16) & 0xFFFF))
#if defined(_WIN64) || defined(__amd64__)
                *
                (13 + (int)((region >> 32) & 0xFFFF)) * (17 + (int)((region >> 48) & 0xFFFF))
#endif
                    ) %
               RecordAccessTracker::SliceSize);
}

int bigHash(size_t region) {
    return hash(region) % RecordAccessTracker::BigHashSize;
}

namespace PointerTable {

/* A "superpage" is a group of 16 contiguous pages that differ
 * only in the low-order 16 bits. This means that there is
 * enough room in the low-order bits to store a bitmap for each
 * page in the superpage.
 */
static const size_t superpageMask = ~0xffffLL;
static const size_t superpageShift = 16;
static const size_t pageSelectorMask = 0xf000LL;  // selects a page in a superpage
static const int pageSelectorShift = 12;

// Tunables
static const int capacity = 128;  // in superpages
static const int bucketSize = 4;  // half cache line
static const int buckets = capacity / bucketSize;

struct Data {
    /** organized similar to a CPU cache
     *  bucketSize-way set associative
     *  least-recently-inserted replacement policy
     */
    size_t _table[buckets][bucketSize];
    long long _lastReset;  // time in millis
};

void reset(Data* data, ClockSource* cs) {
    memset(data->_table, 0, sizeof(data->_table));
    data->_lastReset = cs->now().toMillisSinceEpoch();
}

inline void resetIfNeeded(Data* data, ClockSource* cs) {
    const long long sinceReset = cs->now().toMillisSinceEpoch() - data->_lastReset;
    if (MONGO_unlikely(sinceReset > RecordAccessTracker::RotateTimeSecs * 1000)) {
        reset(data, cs);
    }
}

inline size_t pageBitOf(size_t ptr) {
    return 1LL << ((ptr & pageSelectorMask) >> pageSelectorShift);
}

inline size_t superpageOf(size_t ptr) {
    return ptr & superpageMask;
}

inline size_t bucketFor(size_t ptr) {
    return (ptr >> superpageShift) % buckets;
}

inline bool haveSeenPage(size_t superpage, size_t ptr) {
    return superpage & pageBitOf(ptr);
}

inline void markPageSeen(size_t& superpage, size_t ptr) {
    superpage |= pageBitOf(ptr);
}

/** call this to check a page has been seen yet. */
inline bool seen(Data* data, size_t ptr, ClockSource* cs) {
    resetIfNeeded(data, cs);

    // A bucket contains 4 superpages each containing 16 contiguous pages
    // See above for a more detailed explanation of superpages
    size_t* bucket = data->_table[bucketFor(ptr)];

    for (int i = 0; i < bucketSize; i++) {
        if (superpageOf(ptr) == superpageOf(bucket[i])) {
            if (haveSeenPage(bucket[i], ptr))
                return true;

            markPageSeen(bucket[i], ptr);
            return false;
        }
    }

    // superpage isn't in thread-local cache
    // slide bucket forward and add new superpage at front
    for (int i = bucketSize - 1; i > 0; i--)
        bucket[i] = bucket[i - 1];

    bucket[0] = superpageOf(ptr);
    markPageSeen(bucket[0], ptr);

    return false;
}

Data* getData();

};  // namespace PointerTable

}  // namespace

//
// Slice
//

RecordAccessTracker::Slice::Slice() {
    reset();
}

void RecordAccessTracker::Slice::reset() {
    memset(_data, 0, sizeof(_data));
}

RecordAccessTracker::State RecordAccessTracker::Slice::get(int regionHash,
                                                           size_t region,
                                                           short offset) {
    DEV verify(hash(region) == regionHash);

    Entry* e = _get(regionHash, region, false);
    if (!e)
        return Unk;

    return (e->value & (1ULL << offset)) ? In : Out;
}

bool RecordAccessTracker::Slice::put(int regionHash, size_t region, short offset) {
    DEV verify(hash(region) == regionHash);

    Entry* e = _get(regionHash, region, true);
    if (!e)
        return false;

    e->value |= 1ULL << offset;
    return true;
}

RecordAccessTracker::Entry* RecordAccessTracker::Slice::_get(int start, size_t region, bool add) {
    for (int i = 0; i < MaxChain; i++) {
        int bucket = (start + i) % SliceSize;

        if (_data[bucket].region == 0) {
            if (!add)
                return NULL;

            _data[bucket].region = region;
            return &_data[bucket];
        }

        if (_data[bucket].region == region) {
            return &_data[bucket];
        }
    }

    return NULL;
}

//
// Rolling
//

bool RecordAccessTracker::Rolling::access(size_t region,
                                          short offset,
                                          bool doHalf,
                                          ClockSource* cs) {
    int regionHash = hash(region);

    stdx::lock_guard<SimpleMutex> lk(_lock);

    static int rarelyCount = 0;
    if (rarelyCount++ % (2048 / BigHashSize) == 0) {
        Date_t now = cs->now();

        if (now - _lastRotate > Seconds(static_cast<int64_t>(RotateTimeSecs))) {
            _rotate(cs);
        }
    }

    for (int i = 0; i < NumSlices / (doHalf ? 2 : 1); i++) {
        int pos = (_curSlice + i) % NumSlices;
        State s = _slices[pos].get(regionHash, region, offset);

        if (s == In)
            return true;

        if (s == Out) {
            _slices[pos].put(regionHash, region, offset);
            return false;
        }
    }

    // we weren't in any slice
    // so add to cur
    if (!_slices[_curSlice].put(regionHash, region, offset)) {
        _rotate(cs);
        _slices[_curSlice].put(regionHash, region, offset);
    }
    return false;
}

void RecordAccessTracker::Rolling::updateLastRotate(ClockSource* cs) {
    _lastRotate = cs->now();
}

void RecordAccessTracker::Rolling::_rotate(ClockSource* cs) {
    _curSlice = (_curSlice + 1) % NumSlices;
    _slices[_curSlice].reset();
    updateLastRotate(cs);
}

// These need to be outside the ps namespace due to the way they are defined
#if defined(MONGO_CONFIG_HAVE___THREAD)
__thread PointerTable::Data _pointerTableData;
PointerTable::Data* PointerTable::getData() {
    return &_pointerTableData;
}
#elif defined(MONGO_CONFIG_HAVE___DECLSPEC_THREAD)
__declspec(thread) PointerTable::Data _pointerTableData;
PointerTable::Data* PointerTable::getData() {
    return &_pointerTableData;
}
#else
TSP_DEFINE(PointerTable::Data, _pointerTableData);
PointerTable::Data* PointerTable::getData() {
    return _pointerTableData.getMake();
}
#endif

//
// RecordAccessTracker
//

RecordAccessTracker::RecordAccessTracker(ClockSource* cs)
    : _blockSupported(blockSupported), _clock(cs) {
    reset();
}

void RecordAccessTracker::reset() {
    PointerTable::reset(PointerTable::getData(), _clock);
    _rollingTable.reset(new Rolling[BigHashSize]);
    for (int i = 0; i < BigHashSize; i++) {
        _rollingTable[i].updateLastRotate(_clock);
    }
}

void RecordAccessTracker::markAccessed(const void* record) {
    const size_t page = reinterpret_cast<size_t>(record) >> 12;
    const size_t region = page >> 6;
    const size_t offset = page & 0x3f;

    const bool seen =
        PointerTable::seen(PointerTable::getData(), reinterpret_cast<size_t>(record), _clock);
    if (!seen) {
        _rollingTable[bigHash(region)].access(region, offset, true, _clock);
    }
}


bool RecordAccessTracker::checkAccessedAndMark(const void* record) {
    const size_t page = reinterpret_cast<size_t>(record) >> 12;
    const size_t region = page >> 6;
    const size_t offset = page & 0x3f;

    // This is like the "L1 cache". If we're a miss then we fall through and check the
    // "L2 cache". If we're still a miss, then we defer to a system-specific system
    // call (or give up and return false if deferring to the system call is not enabled).
    if (PointerTable::seen(PointerTable::getData(), reinterpret_cast<size_t>(record), _clock)) {
        return true;
    }

    // We were a miss in the PointerTable. See if we can find 'record' in the Rolling table.
    if (_rollingTable[bigHash(region)].access(region, offset, false, _clock)) {
        return true;
    }

    if (!_blockSupported) {
        // This means we don't fall back to a system call. Instead we assume things aren't
        // in memory. This could mean that we yield too much, but this is much better
        // than the alternative of not yielding through a page fault.
        return false;
    }

    return ProcessInfo::blockInMemory(const_cast<void*>(record));
}

void RecordAccessTracker::disableSystemBlockInMemCheck() {
    _blockSupported = false;
}

}  // namespace mongo
