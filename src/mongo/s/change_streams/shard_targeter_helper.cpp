/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/s/change_streams/shard_targeter_helper.h"

#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"

namespace mongo::change_streams {

void updateActiveShardCursors(Timestamp atClusterTime,
                              const std::vector<ShardId>& newActiveShards,
                              ChangeStreamReaderContext& readerCtx) {
    const auto& currentActiveShardSet = readerCtx.getCurrentlyTargetedDataShards();

    // Convert vector to unordered_set so we can have amortized O(1) lookup.
    // This is beneficial in case there is a higher number of shards.
    const stdx::unordered_set<ShardId> newActiveShardSet(newActiveShards.begin(),
                                                         newActiveShards.end());

    // Determine ShardIds for the cursors to close.
    stdx::unordered_set<ShardId> shardsToCloseCursors;
    for (const auto& currentActiveShard : currentActiveShardSet) {
        if (!newActiveShardSet.contains(currentActiveShard)) {
            shardsToCloseCursors.insert(currentActiveShard);
        }
    }

    if (!shardsToCloseCursors.empty()) {
        readerCtx.closeCursorsOnDataShards(shardsToCloseCursors);
    }

    // Determine ShardIds of the cursors to open.
    stdx::unordered_set<ShardId> shardsToOpenCursors;
    for (const auto& newActiveShard : newActiveShardSet) {
        if (!currentActiveShardSet.contains(newActiveShard)) {
            shardsToOpenCursors.insert(newActiveShard);
        }
    }

    if (!shardsToOpenCursors.empty()) {
        readerCtx.openCursorsOnDataShards(atClusterTime, shardsToOpenCursors);
    }
}

void assertHistoricalPlacementStatusOK(const HistoricalPlacement& placement) {
    tassert(10917001,
            "HistoricalPlacementStatus can not be in the future",
            placement.getStatus() != HistoricalPlacementStatus::FutureClusterTime);
    tassert(10917002,
            "HistoricalPlacementStatus must be OK",
            placement.getStatus() == HistoricalPlacementStatus::OK);
}

void assertHistoricalPlacementHasNoSegment(const HistoricalPlacement& placement) {
    tassert(10922906,
            "Expecting no 'openCursorAt' value in placement response",
            !placement.getOpenCursorAt().has_value());
    tassert(10922907,
            "Expecting no 'nextPlacementChangedAt' value in placement response",
            !placement.getNextPlacementChangedAt().has_value());
}

}  // namespace mongo::change_streams
