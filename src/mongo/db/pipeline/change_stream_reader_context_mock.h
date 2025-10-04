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

#pragma once

#include "mongo/db/pipeline/change_stream_reader_context.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/stdx/unordered_set.h"

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

    const stdx::unordered_set<ShardId>& getCurrentlyTargetedDataShards() const override {
        return currentlyTargetedShards;
    }

    const ChangeStream& getChangeStream() const override {
        return changeStream;
    }

    bool inDegradedMode() const override {
        return degradedMode;
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
        degradedMode = false;
    }

    std::vector<OpenDataShardCall> openCursorsOnDataShardsCalls;
    std::vector<CloseDataShardCall> closeCursorsOnDataShardsCalls;
    std::vector<OpenConfigServerCall> openCursorOnConfigServerCalls;
    int closeCursorOnConfigServerCount = 0;

    stdx::unordered_set<ShardId> currentlyTargetedShards;
    ChangeStream changeStream;
    bool degradedMode = false;
};
}  // namespace mongo
