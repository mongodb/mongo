/**
 *    Copyright (C) 2018 MongoDB Inc.
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
 *    all of the code used source than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/chunk_split_state_driver.h"

#include "mongo/util/assert_util.h"

namespace mongo {

boost::optional<ChunkSplitStateDriver> ChunkSplitStateDriver::tryInitiateSplit(
    std::shared_ptr<ChunkWritesTracker> writesTracker) {
    bool acquiredSplitLock = writesTracker->acquireSplitLock();
    return acquiredSplitLock
        ? boost::optional<ChunkSplitStateDriver>(ChunkSplitStateDriver(writesTracker))
        : boost::none;
}

ChunkSplitStateDriver::ChunkSplitStateDriver(ChunkSplitStateDriver&& source) {
    _writesTracker = source._writesTracker;
    source._writesTracker.reset();
    _splitState = source._splitState;
    // Ensure that the source driver does not cancel the ongoing split
    source._splitState = SplitState::kNotSplitting;
}

ChunkSplitStateDriver::~ChunkSplitStateDriver() {
    if (_splitState == SplitState::kSplitInProgress || _splitState == SplitState::kSplitPrepared) {
        cancelSplit();
    }
}

void ChunkSplitStateDriver::prepareSplit() {
    invariant(_splitState == SplitState::kSplitInProgress);
    _splitState = SplitState::kSplitPrepared;

    _stashedBytesWritten = _getWritesTracker()->getBytesWritten();
    _getWritesTracker()->clearBytesWritten();
}

void ChunkSplitStateDriver::commitSplit() {
    invariant(_splitState == SplitState::kSplitPrepared);
    _splitState = SplitState::kSplitCommitted;
}

void ChunkSplitStateDriver::cancelSplit() {
    invariant(_splitState == SplitState::kSplitInProgress ||
              _splitState == SplitState::kSplitPrepared);
    _splitState = SplitState::kNotSplitting;

    _getWritesTracker()->releaseSplitLock();
    _getWritesTracker()->addBytesWritten(_stashedBytesWritten);
}

std::shared_ptr<ChunkWritesTracker> ChunkSplitStateDriver::_getWritesTracker() {
    auto wt = _writesTracker.lock();
    invariant(wt, "ChunkWritesTracker was destructed before ChunkSplitStateDriver");
    return wt;
}

}  // namespace mongo
