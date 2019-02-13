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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/chunk_split_state_driver.h"

#include "mongo/util/assert_util.h"

namespace mongo {

std::shared_ptr<ChunkSplitStateDriver> ChunkSplitStateDriver::tryInitiateSplit(
    std::shared_ptr<ChunkWritesTracker> writesTracker) {
    invariant(writesTracker);
    bool acquiredSplitLock = writesTracker->acquireSplitLock();
    return acquiredSplitLock
        ? std::shared_ptr<ChunkSplitStateDriver>(new ChunkSplitStateDriver(writesTracker))
        : nullptr;
}

ChunkSplitStateDriver::ChunkSplitStateDriver(ChunkSplitStateDriver&& source) noexcept {
    _writesTracker = source._writesTracker;
    source._writesTracker.reset();

    _stashedBytesWritten = source._stashedBytesWritten;
    source._stashedBytesWritten = 0;

    _splitState = source._splitState;
    source._splitState = SplitState::kNotSplitting;
}

ChunkSplitStateDriver::~ChunkSplitStateDriver() {
    if (_splitState != SplitState::kSplitCommitted) {
        auto wt = _writesTracker.lock();
        if (wt) {
            wt->releaseSplitLock();
            wt->addBytesWritten(_stashedBytesWritten);
        }
    }
}

void ChunkSplitStateDriver::prepareSplit() {
    invariant(_splitState == SplitState::kSplitInProgress);
    _splitState = SplitState::kSplitPrepared;

    auto wt = _writesTracker.lock();
    uassert(50873, "Split interrupted due to chunk metadata change.", wt);
    // Clear bytes written and get the previous bytes written.
    _stashedBytesWritten = wt->clearBytesWritten();
}

void ChunkSplitStateDriver::abandonPrepare() {
    _stashedBytesWritten = 0;
}

void ChunkSplitStateDriver::commitSplit() {
    invariant(_splitState == SplitState::kSplitPrepared);
    _splitState = SplitState::kSplitCommitted;
}

}  // namespace mongo
