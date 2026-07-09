/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/base/counter.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"

namespace mongo {
/**
 * Tracks statistics for the authoritative chunk-operation coordinators (split, merge,
 * mergeAllChunks and moveRange), including their lifecycle outcomes, the volume of chunks they
 * affect, and the local shard-catalog commits they produce.
 */
class MONGO_MOD_PUBLIC ChunkOperationsStatistics {
public:
    // Identifies which chunk-operation coordinator a lifecycle event belongs to.
    enum class ChunkOperationType { kSplitChunk, kMergeChunks, kMergeAllChunks, kMoveRange };

    void report(BSONObjBuilder& builder) const {
        builder.append("countSplitChunkStarted", _countSplitChunkStarted.get());
        builder.append("countSplitChunkCommitted", _countSplitChunkCommitted.get());
        builder.append("countSplitChunkAborted", _countSplitChunkAborted.get());
        builder.append("countMergeChunksStarted", _countMergeChunksStarted.get());
        builder.append("countMergeChunksCommitted", _countMergeChunksCommitted.get());
        builder.append("countMergeChunksAborted", _countMergeChunksAborted.get());
        builder.append("countMergeAllChunksStarted", _countMergeAllChunksStarted.get());
        builder.append("countMergeAllChunksCommitted", _countMergeAllChunksCommitted.get());
        builder.append("countMergeAllChunksAborted", _countMergeAllChunksAborted.get());
        builder.append("countMoveRangeStarted", _countMoveRangeStarted.get());
        builder.append("countMoveRangeCommitted", _countMoveRangeCommitted.get());
        builder.append("countMoveRangeAborted", _countMoveRangeAborted.get());

        builder.append("countSplitChunkResultingChunks", _countSplitChunkResultingChunks.get());
        builder.append("countMergeAllChunksMerged", _countMergeAllChunksMerged.get());
        builder.append("countMoveRangeChunksMoved", _countMoveRangeChunksMoved.get());
        builder.append("countMoveRangeFirstChunkReceived", _countMoveRangeFirstChunkReceived.get());

        builder.append("countLocalChunkOperationsMetadataCommits",
                       _countLocalChunkOperationsMetadataCommits.get());
        builder.append("countChunksCommittedToShardCatalog",
                       _countChunksCommittedToShardCatalog.get());
    }

    void registerStarted(ChunkOperationType type) {
        switch (type) {
            case ChunkOperationType::kSplitChunk:
                _countSplitChunkStarted.incrementRelaxed();
                break;
            case ChunkOperationType::kMergeChunks:
                _countMergeChunksStarted.incrementRelaxed();
                break;
            case ChunkOperationType::kMergeAllChunks:
                _countMergeAllChunksStarted.incrementRelaxed();
                break;
            case ChunkOperationType::kMoveRange:
                _countMoveRangeStarted.incrementRelaxed();
                break;
        }
    }

    void registerCommitted(ChunkOperationType type) {
        switch (type) {
            case ChunkOperationType::kSplitChunk:
                _countSplitChunkCommitted.incrementRelaxed();
                break;
            case ChunkOperationType::kMergeChunks:
                _countMergeChunksCommitted.incrementRelaxed();
                break;
            case ChunkOperationType::kMergeAllChunks:
                _countMergeAllChunksCommitted.incrementRelaxed();
                break;
            case ChunkOperationType::kMoveRange:
                _countMoveRangeCommitted.incrementRelaxed();
                break;
        }
    }

    void registerAborted(ChunkOperationType type) {
        switch (type) {
            case ChunkOperationType::kSplitChunk:
                _countSplitChunkAborted.incrementRelaxed();
                break;
            case ChunkOperationType::kMergeChunks:
                _countMergeChunksAborted.incrementRelaxed();
                break;
            case ChunkOperationType::kMergeAllChunks:
                _countMergeAllChunksAborted.incrementRelaxed();
                break;
            case ChunkOperationType::kMoveRange:
                _countMoveRangeAborted.incrementRelaxed();
                break;
        }
    }

    void registerSplitChunkResultingChunks(long long numChunks) {
        _countSplitChunkResultingChunks.incrementRelaxed(numChunks);
    }

    void registerMergeAllChunksMerged(long long numChunks) {
        _countMergeAllChunksMerged.incrementRelaxed(numChunks);
    }

    void registerMoveRangeChunksMoved(long long numChunks) {
        _countMoveRangeChunksMoved.incrementRelaxed(numChunks);
    }

    void registerMoveRangeFirstChunkReceived() {
        _countMoveRangeFirstChunkReceived.incrementRelaxed();
    }

    void registerLocalChunkOperationsMetadataCommit(long long numChunks) {
        _countLocalChunkOperationsMetadataCommits.incrementRelaxed();
        _countChunksCommittedToShardCatalog.incrementRelaxed(numChunks);
    }

private:
    // Split chunk operations begun, committed, and aborted by the SplitChunkCoordinator.
    Counter64 _countSplitChunkStarted;
    Counter64 _countSplitChunkCommitted;
    Counter64 _countSplitChunkAborted;
    // Merge chunks operations begun, committed, and aborted by the MergeChunksCoordinator.
    Counter64 _countMergeChunksStarted;
    Counter64 _countMergeChunksCommitted;
    Counter64 _countMergeChunksAborted;
    // MergeAllChunks operations begun, committed, and aborted by the MergeAllChunksCoordinator.
    Counter64 _countMergeAllChunksStarted;
    Counter64 _countMergeAllChunksCommitted;
    Counter64 _countMergeAllChunksAborted;
    // MoveRange operations begun, committed, and aborted by the MoveRangeCoordinator. These
    // intentionally overlap the legacy countDonorMoveChunk* counters, kept here for a uniform
    // per-operation view of all chunk operations.
    Counter64 _countMoveRangeStarted;
    Counter64 _countMoveRangeCommitted;
    Counter64 _countMoveRangeAborted;

    // New chunks produced by committed split operations (split points + 1 per split).
    Counter64 _countSplitChunkResultingChunks;
    // Chunks collapsed away by committed mergeAllChunks operations.
    Counter64 _countMergeAllChunksMerged;
    // Chunks (ranges) relocated by committed moveRange operations.
    Counter64 _countMoveRangeChunksMoved;
    // Committed moveRange operations that delivered a shard its first chunk for the collection.
    Counter64 _countMoveRangeFirstChunkReceived;

    // Invocations of commitChunkOperationsMetadataLocally (the incremental shard-catalog commit
    // behind the _shardsvrCommitChunkOperationsMetadata command).
    Counter64 _countLocalChunkOperationsMetadataCommits;
    // Total chunk documents written by that incremental shard-catalog commit path.
    Counter64 _countChunksCommittedToShardCatalog;
};
}  // namespace mongo
