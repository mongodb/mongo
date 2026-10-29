// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/change_stream_reader_context.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"

namespace mongo {
class ChangeStreamReaderContextMock : public ChangeStreamReaderContext {
public:
    struct OpenDataShardCall {
        Timestamp atClusterTime;
        stdx::unordered_set<ShardId> shardSet;
    };

    struct CloseDataShardCall {
        stdx::unordered_set<ShardId> shardSet;
    };

    struct OpenConfigServerCall {
        Timestamp atClusterTime;
    };

    explicit ChangeStreamReaderContextMock(ChangeStream changeStream)
        : changeStream(changeStream) {}

    void openCursorsOnDataShards(Timestamp atClusterTime,
                                 const stdx::unordered_set<ShardId>& shardSet) override {
        openCursorsOnDataShardsCalls.emplace_back(OpenDataShardCall{atClusterTime, shardSet});
    }

    void closeCursorsOnDataShards(const stdx::unordered_set<ShardId>& shardSet) override {
        closeCursorsOnDataShardsCalls.emplace_back(CloseDataShardCall{shardSet});
    }

    void openCursorOnConfigServer(Timestamp atClusterTime) override {
        openCursorOnConfigServerCalls.emplace_back(OpenConfigServerCall{atClusterTime});
    }

    void closeCursorOnConfigServer() override {
        ++closeCursorOnConfigServerCount;
    }

    bool isCursorOnConfigServerOpen() const override {
        return cursorOnConfigServerOpen;
    }

    const stdx::unordered_set<ShardId>& getCurrentlyTargetedDataShards() const override {
        return currentlyTargetedShards;
    }

    const ChangeStream& getChangeStream() const override {
        return changeStream;
    }

    bool inDegradedMode() const override {
        return degradedMode;
    }

    void setCursorOnConfigServerOpen(bool val) {
        cursorOnConfigServerOpen = val;
    }

    void setTargetedShards(stdx::unordered_set<ShardId> val) {
        currentlyTargetedShards = std::move(val);
    }

    void setDegradedMode(bool degraded) {
        degradedMode = degraded;
    }

    void reset() {
        openCursorsOnDataShardsCalls.clear();
        closeCursorsOnDataShardsCalls.clear();
        openCursorOnConfigServerCalls.clear();
        closeCursorOnConfigServerCount = 0;
        currentlyTargetedShards.clear();
        cursorOnConfigServerOpen = false;
        degradedMode = false;
    }

    const ChangeStream changeStream;

    // State changes caused by calls made to open/close cursors.
    std::vector<OpenDataShardCall> openCursorsOnDataShardsCalls;
    std::vector<CloseDataShardCall> closeCursorsOnDataShardsCalls;
    std::vector<OpenConfigServerCall> openCursorOnConfigServerCalls;
    int closeCursorOnConfigServerCount = 0;

    // Initial state, to be set before making calls to open/close cursors.
    stdx::unordered_set<ShardId> currentlyTargetedShards;
    bool cursorOnConfigServerOpen = false;
    bool degradedMode = false;
};
}  // namespace mongo
