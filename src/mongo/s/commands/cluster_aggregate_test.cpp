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

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/keys_collection_client_sharded.h"
#include "mongo/db/keys_collection_manager.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/s/catalog_cache_test_fixture.h"
#include "mongo/s/commands/cluster_aggregate.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

const size_t numShards = 2;

const NamespaceString kNss = NamespaceString("test", "coll");

using InspectionCallback = stdx::function<void(const executor::RemoteCommandRequest& request)>;

class ClusterAggregateTest : public CatalogCacheTestFixture {
protected:
    const BSONObj kAggregateCmdTargeted{
        fromjson("{aggregate: 'coll', pipeline: [{$match: {_id: 0}}], explain: false, "
                 "allowDiskUse: false, fromMongos: true, "
                 "cursor: {batchSize: 10}, maxTimeMS: 100, readConcern: {level: 'snapshot'}}")};

    const BSONObj kAggregateCmdScatterGather{fromjson(
        "{aggregate: 'coll', pipeline: [], explain: false, allowDiskUse: false, fromMongos: true, "
        "cursor: {batchSize: 10}, readConcern: {level: 'snapshot'}}")};

    void setUp() {
        CatalogCacheTestFixture::setUp();
        CatalogCacheTestFixture::setupNShards(numShards);

        // Set up a logical clock with an initial time.
        auto logicalClock = stdx::make_unique<LogicalClock>(serviceContext());
        LogicalTime initialTime(Timestamp(10, 1));
        logicalClock->setClusterTimeFromTrustedSource(initialTime);
        LogicalClock::set(serviceContext(), std::move(logicalClock));

        auto keysCollectionClient = stdx::make_unique<KeysCollectionClientSharded>(
            Grid::get(operationContext())->catalogClient());

        auto keyManager = std::make_shared<KeysCollectionManager>(
            "dummy", std::move(keysCollectionClient), Seconds(KeysRotationIntervalSec));

        auto validator = stdx::make_unique<LogicalTimeValidator>(keyManager);
        LogicalTimeValidator::set(serviceContext(), std::move(validator));

        // ReadConcern 'snapshot' is only supported with test commands enabled.
        setTestCommandsEnabled(true);
    }

    // The index of the shard expected to receive the response is used to prevent different shards
    // from returning documents with the same shard key. This is expected to be 0 for queries
    // targeting one shard.
    void expectAggReturnsSuccess(int shardIndex) {
        onCommandForPoolExecutor([shardIndex](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(kNss.coll(), request.cmdObj.firstElement().valueStringData());

            std::vector<BSONObj> batch = {BSON("_id" << shardIndex)};
            CursorResponse cursorResponse(kNss, CursorId(0), batch);
            return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
        });
    }

    void expectAggInspectRequest(int shardIndex, InspectionCallback cb) {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(kNss.coll(), request.cmdObj.firstElement().valueStringData());

            cb(request);

            std::vector<BSONObj> batch = {BSON("_id" << shardIndex)};
            CursorResponse cursorResponse(kNss, CursorId(0), batch);
            return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
        });
    }

    void expectAggReturnsError(ErrorCodes::Error code) {
        onCommandForPoolExecutor([code](const executor::RemoteCommandRequest& request) {
            BSONObjBuilder resBob;
            CommandHelpers::appendCommandStatusNoThrow(resBob, Status(code, "dummy error"));
            return resBob.obj();
        });
    }

    DbResponse runAggregateCommand(BSONObj aggCmd) {
        // Create a new client/operation context per command, and setup a test session ID and
        // transaction number.
        auto client = serviceContext()->makeClient("ClusterAggClient");
        auto opCtx = client->makeOperationContext();
        opCtx->setLogicalSessionId(makeLogicalSessionIdForTest());
        opCtx->setTxnNumber(1);

        const auto opMsgRequest = OpMsgRequest::fromDBAndBody(kNss.db(), aggCmd);

        return Strategy::clientCommand(opCtx.get(), opMsgRequest.serialize());
    }

    void runAggCommandSuccessful(BSONObj cmd, bool isTargeted) {
        auto future = launchAsync([&] {
            // Shouldn't throw.
            runAggregateCommand(cmd);
        });

        size_t numMocks = isTargeted ? 1 : numShards;
        for (size_t i = 0; i < numMocks; i++) {
            expectAggReturnsSuccess(i % numShards);
        }

        future.timed_get(kFutureTimeout);
    }

    void runAggCommandOneError(BSONObj cmd, ErrorCodes::Error code, bool isTargeted) {
        auto future = launchAsync([&] {
            // Shouldn't throw.
            runAggregateCommand(cmd);
        });

        size_t numMocks = isTargeted ? 1 : numShards;
        for (size_t i = 0; i < numMocks; i++) {
            expectAggReturnsError(code);
        }
        for (size_t i = 0; i < numMocks; i++) {
            expectAggReturnsSuccess(i % numShards);
        }

        future.timed_get(kFutureTimeout);
    }

    void runCommandInspectRequests(BSONObj cmd, InspectionCallback cb, bool isTargeted) {
        auto future = launchAsync([&] { runAggregateCommand(cmd); });

        size_t numMocks = isTargeted ? 1 : numShards;
        for (size_t i = 0; i < numMocks; i++) {
            expectAggInspectRequest(i % numShards, cb);
        }

        future.timed_get(kFutureTimeout);
    }

    void runAggCommandMaxErrors(BSONObj cmd, ErrorCodes::Error code, bool isTargeted) {
        auto future = launchAsync([&] { runAggregateCommand(cmd); });

        size_t numRetries =
            isTargeted ? kMaxNumStaleVersionRetries : kMaxNumStaleVersionRetries * numShards;
        for (size_t i = 0; i < numRetries; i++) {
            expectAggReturnsError(code);
        }

        future.timed_get(kFutureTimeout);
    }
};

TEST_F(ClusterAggregateTest, NoErrors) {
    loadRoutingTableWithTwoChunksAndTwoShards(kNss);
    log() << "Target one shard: " << kAggregateCmdTargeted;
    // Target one shard.
    runAggCommandSuccessful(kAggregateCmdTargeted, true);

    log() << "Target all shards: " << kAggregateCmdScatterGather;
    // Target all shards.
    runAggCommandSuccessful(kAggregateCmdScatterGather, false);
}

// Verify aggregate through mongos will retry on a snapshot error.
TEST_F(ClusterAggregateTest, RetryOnSnapshotError) {
    loadRoutingTableWithTwoChunksAndTwoShards(kNss);

    // Target one shard.
    runAggCommandOneError(kAggregateCmdTargeted, ErrorCodes::SnapshotUnavailable, true);
    runAggCommandOneError(kAggregateCmdTargeted, ErrorCodes::SnapshotTooOld, true);

    // Target all shards
    runAggCommandOneError(kAggregateCmdScatterGather, ErrorCodes::SnapshotUnavailable, false);
    runAggCommandOneError(kAggregateCmdScatterGather, ErrorCodes::SnapshotTooOld, false);
}

TEST_F(ClusterAggregateTest, AttachesAtClusterTimeForSnapshotReadConcern) {
    loadRoutingTableWithTwoChunksAndTwoShards(kNss);

    auto containsAtClusterTime = [](const executor::RemoteCommandRequest& request) {
        ASSERT(!request.cmdObj["readConcern"]["atClusterTime"].eoo());
    };

    // Target one shard.
    runCommandInspectRequests(kAggregateCmdTargeted, containsAtClusterTime, true);

    // Target all shards.
    runCommandInspectRequests(kAggregateCmdScatterGather, containsAtClusterTime, false);
}

// Verify aggregate commands will retry up to its max retry attempts on snapshot errors
// then return the final error it receives.
TEST_F(ClusterAggregateTest, MaxRetriesSnapshotErrors) {
    loadRoutingTableWithTwoChunksAndTwoShards(kNss);

    // Target one shard.
    runAggCommandMaxErrors(kAggregateCmdTargeted, ErrorCodes::SnapshotUnavailable, true);
    runAggCommandMaxErrors(kAggregateCmdTargeted, ErrorCodes::SnapshotTooOld, true);

    // Target all shards
    runAggCommandMaxErrors(kAggregateCmdScatterGather, ErrorCodes::SnapshotUnavailable, false);
    runAggCommandMaxErrors(kAggregateCmdScatterGather, ErrorCodes::SnapshotTooOld, false);
}

}  // namespace
}  // namespace mongo
