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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/mmap_v1/dur_recovery_unit.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <string>

#include "mongo/db/storage/mmap_v1/dur.h"
#include "mongo/db/storage/mmap_v1/durable_mapped_file.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {

DurRecoveryUnit::DurRecoveryUnit()
    : _writeCount(0), _writeBytes(0), _inUnitOfWork(false), _rollbackWritesDisabled(false) {}

void DurRecoveryUnit::beginUnitOfWork(OperationContext* opCtx) {
    invariant(!_inUnitOfWork);
    _inUnitOfWork = true;
}

void DurRecoveryUnit::commitUnitOfWork() {
    invariant(_inUnitOfWork);

    commitChanges();

    // global journal flush opportunity
    getDur().commitIfNeeded();

    resetChanges();
}

void DurRecoveryUnit::abortUnitOfWork() {
    invariant(_inUnitOfWork);

    rollbackChanges();
    resetChanges();
}

void DurRecoveryUnit::abandonSnapshot() {
    invariant(!_inUnitOfWork);
    // no-op since we have no transaction
}

void DurRecoveryUnit::commitChanges() {
    if (getDur().isDurable())
        markWritesForJournaling();

    try {
        for (Changes::const_iterator it = _changes.begin(), end = _changes.end(); it != end; ++it) {
            (*it)->commit();
        }
    } catch (...) {
        std::terminate();
    }
}

void DurRecoveryUnit::markWritesForJournaling() {
    if (!_writeCount)
        return;

    typedef std::pair<void*, unsigned> Intent;
    std::vector<Intent> intents;
    const size_t numStoredWrites = _initialWrites.size() + _mergedWrites.size();
    intents.reserve(numStoredWrites);

    // Show very large units of work at LOG(1) level as they may hint at performance issues
    const int logLevel = (_writeCount > 100 * 1000 || _writeBytes > 50 * 1024 * 1024) ? 1 : 3;

    LOG(logLevel) << _writeCount << " writes (" << _writeBytes / 1024 << " kB) covered by "
                  << numStoredWrites << " pre-images (" << _preimageBuffer.size() / 1024 << " kB) ";

    // orders the initial, unmerged writes, by address so we can coalesce overlapping and
    // adjacent writes
    std::sort(_initialWrites.begin(), _initialWrites.end());

    if (!_initialWrites.empty()) {
        intents.push_back(std::make_pair(_initialWrites.front().addr, _initialWrites.front().len));
        for (InitialWrites::iterator it = (_initialWrites.begin() + 1), end = _initialWrites.end();
             it != end;
             ++it) {
            Intent& lastIntent = intents.back();
            char* lastEnd = static_cast<char*>(lastIntent.first) + lastIntent.second;
            if (it->addr <= lastEnd) {
                // overlapping or adjacent, so extend.
                ptrdiff_t extendedLen = (it->end()) - static_cast<char*>(lastIntent.first);
                lastIntent.second = std::max(lastIntent.second, unsigned(extendedLen));
            } else {
                // not overlapping, so create a new intent
                intents.push_back(std::make_pair(it->addr, it->len));
            }
        }
    }

    MergedWrites::iterator it = _mergedWrites.begin();
    if (it != _mergedWrites.end()) {
        intents.push_back(std::make_pair(it->addr, it->len));
        while (++it != _mergedWrites.end()) {
            // Check the property that write intents are sorted and don't overlap.
            invariant(it->addr >= intents.back().first);
            Intent& lastIntent = intents.back();
            char* lastEnd = static_cast<char*>(lastIntent.first) + lastIntent.second;
            if (it->addr == lastEnd) {
                //  adjacent, so extend.
                lastIntent.second += it->len;
            } else {
                // not overlapping, so create a new intent
                invariant(it->addr > lastEnd);
                intents.push_back(std::make_pair(it->addr, it->len));
            }
        }
    }
    LOG(logLevel) << _mergedWrites.size() << " pre-images "
                  << "coalesced into " << intents.size() << " write intents";

    getDur().declareWriteIntents(intents);
}

void DurRecoveryUnit::resetChanges() {
    _writeCount = 0;
    _writeBytes = 0;
    _initialWrites.clear();
    _mergedWrites.clear();
    _changes.clear();
    _preimageBuffer.clear();
    _rollbackWritesDisabled = false;
    _inUnitOfWork = false;
}

void DurRecoveryUnit::rollbackChanges() {
    // First rollback disk writes, then Changes. This matches behavior in other storage engines
    // that either rollback a transaction or don't write a writebatch.

    if (_rollbackWritesDisabled) {
        LOG(2) << "   ***** NOT ROLLING BACK " << _writeCount << " disk writes";
    } else {
        LOG(2) << "   ***** ROLLING BACK " << _writeCount << " disk writes";

        // First roll back the merged writes. These have no overlap or ordering requirement
        // other than needing to be rolled back before all _initialWrites.
        for (MergedWrites::iterator it = _mergedWrites.begin(); it != _mergedWrites.end(); ++it) {
            _preimageBuffer.copy(it->addr, it->len, it->offset);
        }

        // Then roll back the initial writes in LIFO order, as these might have overlaps.
        for (InitialWrites::reverse_iterator rit = _initialWrites.rbegin();
             rit != _initialWrites.rend();
             ++rit) {
            _preimageBuffer.copy(rit->addr, rit->len, rit->offset);
        }
    }

    LOG(2) << "   ***** ROLLING BACK " << (_changes.size()) << " custom changes";

    try {
        for (int i = _changes.size() - 1; i >= 0; i--) {
            LOG(2) << "CUSTOM ROLLBACK " << demangleName(typeid(*_changes[i]));
            _changes[i]->rollback();
        }
    } catch (...) {
        std::terminate();
    }
}

bool DurRecoveryUnit::waitUntilDurable() {
    invariant(!_inUnitOfWork);
    return getDur().waitUntilDurable();
}

void DurRecoveryUnit::mergingWritingPtr(char* addr, size_t len) {
    // The invariant is that all writes are non-overlapping and non-empty. So, a single
    // writingPtr call may result in a number of new segments added. At this point, we cannot
    // in general merge adjacent writes, as that would require inefficient operations on the
    // preimage buffer.

    MergedWrites::iterator coveringWrite = _mergedWrites.upper_bound(Write(addr, 0, 0));

    char* const end = addr + len;
    while (addr < end) {
        dassert(coveringWrite == _mergedWrites.end() || coveringWrite->end() > addr);

        // Determine whether addr[0] is already covered by a write or not.
        // If covered, adjust addr and len to exclude the covered run from addr[0] onwards.

        if (coveringWrite != _mergedWrites.end()) {
            char* const cwEnd = coveringWrite->end();

            if (coveringWrite->addr <= addr) {
                // If the begin of the covering write at or before addr[0], addr[0] is covered.
                // While the existing pre-image will not generally be the same as the data
                // being written now, during rollback only the oldest pre-image matters.

                if (end <= cwEnd) {
                    break;  // fully covered
                }

                addr = cwEnd;
                coveringWrite++;
                dassert(coveringWrite == _mergedWrites.end() || coveringWrite->addr >= cwEnd);
            }
        }
        dassert(coveringWrite == _mergedWrites.end() || coveringWrite->end() > addr);

        // If the next coveringWrite overlaps, adjust the end of the uncovered region.
        char* uncoveredEnd = end;
        if (coveringWrite != _mergedWrites.end() && coveringWrite->addr < end) {
            uncoveredEnd = coveringWrite->addr;
        }

        const size_t uncoveredLen = uncoveredEnd - addr;
        if (uncoveredLen) {
            // We are writing to a region that hasn't been declared previously.
            _mergedWrites.insert(Write(addr, uncoveredLen, _preimageBuffer.size()));

            // Windows requires us to adjust the address space *before* we write to anything.
            privateViews.makeWritable(addr, uncoveredLen);

            if (!_rollbackWritesDisabled) {
                _preimageBuffer.append(addr, uncoveredLen);
            }
            addr = uncoveredEnd;
        }
    }
}

void* DurRecoveryUnit::writingPtr(void* addr, size_t len) {
    invariant(_inUnitOfWork);

    if (len == 0) {
        return addr;  // Don't need to do anything for empty ranges.
    }

    invariant(len < size_t(std::numeric_limits<int>::max()));

    _writeCount++;
    _writeBytes += len;
    char* const data = static_cast<char*>(addr);

    //  The initial writes are stored in a faster, but less memory-efficient way. This will
    //  typically be enough for simple operations, where the extra cost of incremental
    //  coalescing and merging would be too much. For larger writes, more redundancy is
    //  is expected, so the cost of checking for duplicates is offset by savings in copying
    //  and allocating preimage buffers. Total memory use of the preimage buffer may be up to
    //  kMaxUnmergedPreimageBytes larger than the amount memory covered by the write intents.

    const size_t kMaxUnmergedPreimageBytes = kDebugBuild ? 16 * 1024 : 10 * 1024 * 1024;

    if (_preimageBuffer.size() + len > kMaxUnmergedPreimageBytes) {
        mergingWritingPtr(data, len);

        // After a merged write, no more initial writes can occur or there would be an
        // ordering violation during rollback. So, ensure that the if-condition will be true
        // for any future write regardless of length. This is true now because
        // mergingWritingPtr also will store its first write in _preimageBuffer as well.
        invariant(_preimageBuffer.size() >= kMaxUnmergedPreimageBytes);

        return addr;
    }

    // Windows requires us to adjust the address space *before* we write to anything.
    privateViews.makeWritable(data, len);

    _initialWrites.push_back(Write(data, len, _preimageBuffer.size()));

    if (!_rollbackWritesDisabled) {
        _preimageBuffer.append(data, len);
    }

    return addr;
}

void DurRecoveryUnit::setRollbackWritesDisabled() {
    invariant(_inUnitOfWork);
    _rollbackWritesDisabled = true;
}

void DurRecoveryUnit::registerChange(Change* change) {
    invariant(_inUnitOfWork);
    _changes.push_back(change);
}

}  // namespace mongo
