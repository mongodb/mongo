/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/s/commands/cluster_command_test_fixture.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

class ClusterDistinctTest : public ClusterCommandTestFixture {
protected:
    const Timestamp kAfterClusterTime = Timestamp(50, 2);

    const BSONObj kDistinctCmdTargeted{
        fromjson("{distinct: 'coll', key: 'x', query: {'_id': {$lt: -1}}, autocommit: false, "
                 "txnNumber: NumberLong(1), startTransaction: true}")};

    const BSONObj kDistinctCmdScatterGather{
        fromjson("{distinct: 'coll', key: '_id', autocommit: false, txnNumber: NumberLong(1), "
                 "startTransaction: true}")};

    BSONObj appendLogicalSessionIdAndSnapshotReadConcern(BSONObj cmdObj,
                                                         bool includeAfterClusterTime) {
        BSONObjBuilder bob(cmdObj);
        bob.append("lsid", makeLogicalSessionIdForTest().toBSON());
        BSONObjBuilder readConcernBob =
            bob.subobjStart(repl::ReadConcernArgs::kReadConcernFieldName);
        readConcernBob.append("level", "snapshot");
        if (includeAfterClusterTime) {
            readConcernBob.append("afterClusterTime", kAfterClusterTime);
        }
        readConcernBob.doneFast();
        return bob.obj();
    }

    BSONObj distinctCmdTargeted(bool includeAfterClusterTime = false) {
        return appendLogicalSessionIdAndSnapshotReadConcern(kDistinctCmdTargeted,
                                                            includeAfterClusterTime);
    }

    BSONObj distinctCmdScatterGather(bool includeAfterClusterTime = false) {
        return appendLogicalSessionIdAndSnapshotReadConcern(kDistinctCmdScatterGather,
                                                            includeAfterClusterTime);
    }

    void expectInspectRequest(int shardIndex, InspectionCallback cb) override {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(kNss.coll(), request.cmdObj.firstElement().valueStringData());
            cb(request);
            return BSON("values" << BSON_ARRAY(shardIndex));
        });
    }

    void expectReturnsSuccess(int shardIndex) override {
        onCommandForPoolExecutor([this, shardIndex](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(kNss.coll(), request.cmdObj.firstElement().valueStringData());
            return BSON("values" << BSON_ARRAY(shardIndex));
        });
    }
};

TEST_F(ClusterDistinctTest, NoErrors) {
    loadRoutingTableWithTwoChunksAndTwoShards(kNss);

    // Target one shard.
    runCommandSuccessful(distinctCmdTargeted(), true);

    // Target all shards.
    runCommandSuccessful(distinctCmdScatterGather(), false);
}

// Verify distinct through mongos will retry on a snapshot error.
TEST_F(ClusterDistinctTest, RetryOnSnapshotError) {
    loadRoutingTableWithTwoChunksAndTwoShards(kNss);

    // Target one shard.
    runCommandOneError(distinctCmdTargeted(), ErrorCodes::SnapshotUnavailable, true);
    runCommandOneError(distinctCmdTargeted(), ErrorCodes::SnapshotTooOld, true);

    // Target all shards
    runCommandOneError(distinctCmdScatterGather(), ErrorCodes::SnapshotUnavailable, false);
    runCommandOneError(distinctCmdScatterGather(), ErrorCodes::SnapshotTooOld, false);
}

// Verify distinct commands will retry up to its max retry attempts on snapshot errors
// then return the final error it receives.
TEST_F(ClusterDistinctTest, MaxRetriesSnapshotErrors) {
    loadRoutingTableWithTwoChunksAndTwoShards(kNss);

    // Target one shard.
    runCommandMaxErrors(distinctCmdTargeted(), ErrorCodes::SnapshotUnavailable, true);
    runCommandMaxErrors(distinctCmdTargeted(), ErrorCodes::SnapshotTooOld, true);

    // Target all shards
    runCommandMaxErrors(distinctCmdScatterGather(), ErrorCodes::SnapshotUnavailable, false);
    runCommandMaxErrors(distinctCmdScatterGather(), ErrorCodes::SnapshotTooOld, false);
}

TEST_F(ClusterDistinctTest, AttachesAtClusterTimeForSnapshotReadConcern) {
    loadRoutingTableWithTwoChunksAndTwoShards(kNss);

    auto containsAtClusterTime = [](const executor::RemoteCommandRequest& request) {
        ASSERT(!request.cmdObj["readConcern"]["atClusterTime"].eoo());
    };

    // Target one shard.
    runCommandInspectRequests(distinctCmdTargeted(), containsAtClusterTime, true);

    // Target all shards.
    runCommandInspectRequests(distinctCmdScatterGather(), containsAtClusterTime, false);
}

TEST_F(ClusterDistinctTest, SnapshotReadConcernWithAfterClusterTime) {
    loadRoutingTableWithTwoChunksAndTwoShards(kNss);

    auto containsAtClusterTimeNoAfterClusterTime =
        [&](const executor::RemoteCommandRequest& request) {
            ASSERT(!request.cmdObj["readConcern"]["atClusterTime"].eoo());
            ASSERT(request.cmdObj["readConcern"]["afterClusterTime"].eoo());

            // The chosen atClusterTime should be greater than or equal to the request's
            // afterClusterTime.
            ASSERT_GTE(LogicalTime(request.cmdObj["readConcern"]["atClusterTime"].timestamp()),
                       LogicalTime(kAfterClusterTime));
        };

    // Target one shard.
    runCommandInspectRequests(distinctCmdTargeted(true /*includeAfterClusterTime*/),
                              containsAtClusterTimeNoAfterClusterTime,
                              true);

    // Target all shards.
    runCommandInspectRequests(distinctCmdScatterGather(true /*includeAfterClusterTime*/),
                              containsAtClusterTimeNoAfterClusterTime,
                              false);
}

}  // namespace
}  // namespace mongo
