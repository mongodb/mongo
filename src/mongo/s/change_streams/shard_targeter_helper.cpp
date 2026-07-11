// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

DataToShardsAllocationQueryService* getDataToShardsAllocationQueryService(OperationContext* opCtx) {
    DataToShardsAllocationQueryService* dataToShardsAllocationQueryService =
        DataToShardsAllocationQueryService::get(opCtx);
    tassert(11901800,
            "expecting DataToShardsAllocationQueryService to be available",
            dataToShardsAllocationQueryService);
    return dataToShardsAllocationQueryService;
}

}  // namespace mongo::change_streams
