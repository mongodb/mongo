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

#include "mongo/platform/basic.h"

#include "mongo/db/commands.h"
#include "mongo/db/keys_collection_client_sharded.h"
#include "mongo/db/keys_collection_manager.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_session_cache_noop.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/query_request.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/s/catalog_cache_test_fixture.h"
#include "mongo/s/commands/strategy.h"
#include "mongo/s/query/cluster_find.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

const size_t numShards = 2;

const NamespaceString kNss = NamespaceString("test", "coll");

const auto kFindCmdScatterGather = BSON("find" << kNss.coll());
const auto kFindCmdTargeted = BSON("find" << kNss.coll() << "filter" << BSON("_id" << 0));

const LogicalTime kInMemoryLogicalTime(Timestamp(10, 1));

const LogicalTime kAfterClusterTime(Timestamp(50, 2));

using InspectionCallback = stdx::function<void(const executor::RemoteCommandRequest& request)>;

BSONObj appendSnapshotReadConcernAndTxnOptions(BSONObj cmdObj,
                                               bool includeAfterClusterTime = false) {
    BSONObjBuilder bob(cmdObj);
    bob.append("autocommit", false);
    bob.append("txnNumber", TxnNumber(1));
    bob.append("startTransaction", true);
    bob.append("lsid", makeLogicalSessionIdForTest().toBSON());
    BSONObjBuilder readConcernBob = bob.subobjStart(repl::ReadConcernArgs::kReadConcernFieldName);
    readConcernBob.append("level", "snapshot");
    if (includeAfterClusterTime) {
        readConcernBob.append("afterClusterTime", kAfterClusterTime.asTimestamp());
    }
    readConcernBob.doneFast();
    return bob.obj();
}

class ClusterFindTest : public CatalogCacheTestFixture {
protected:
    void setUp() {
        CatalogCacheTestFixture::setUp();
        CatalogCacheTestFixture::setupNShards(numShards);

        // Set up a logical clock with an initial time.
        auto logicalClock = stdx::make_unique<LogicalClock>(getServiceContext());
        logicalClock->setClusterTimeFromTrustedSource(kInMemoryLogicalTime);
        LogicalClock::set(getServiceContext(), std::move(logicalClock));

        auto keysCollectionClient = stdx::make_unique<KeysCollectionClientSharded>(
            Grid::get(operationContext())->catalogClient());

        auto keyManager = std::make_shared<KeysCollectionManager>(
            "dummy", std::move(keysCollectionClient), Seconds(KeysRotationIntervalSec));

        auto validator = stdx::make_unique<LogicalTimeValidator>(keyManager);
        LogicalTimeValidator::set(getServiceContext(), std::move(validator));

        LogicalSessionCache::set(getServiceContext(), stdx::make_unique<LogicalSessionCacheNoop>());
    }

    // The index of the shard expected to receive the response is used to prevent different shards
    // from returning documents with the same shard key. This is expected to be 0 for queries
    // targeting one shard.
    void expectFindReturnsSuccess(int shardIndex) {
        onCommandForPoolExecutor([shardIndex](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(kNss.coll(), request.cmdObj.firstElement().valueStringData());

            std::vector<BSONObj> batch = {BSON("_id" << shardIndex)};
            CursorResponse cursorResponse(kNss, CursorId(0), batch);
            return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
        });
    }

    void expectFindInspectRequest(int shardIndex, InspectionCallback cb) {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(kNss.coll(), request.cmdObj.firstElement().valueStringData());

            cb(request);

            std::vector<BSONObj> batch = {BSON("_id" << shardIndex)};
            CursorResponse cursorResponse(kNss, CursorId(0), batch);
            return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
        });
    }

    void expectFindReturnsError(ErrorCodes::Error code) {
        onCommandForPoolExecutor([code](const executor::RemoteCommandRequest& request) {
            BSONObjBuilder resBob;
            CommandHelpers::appendCommandStatusNoThrow(resBob, Status(code, "dummy error"));
            return resBob.obj();
        });
    }

    std::unique_ptr<CanonicalQuery> makeCanonicalQueryFromFindCommand(BSONObj findCmd) {
        const NamespaceString nss(CommandHelpers::parseNsCollectionRequired(kNss.db(), findCmd));

        const bool isExplain = false;
        auto qr = QueryRequest::makeFromFindCommand(nss, findCmd, isExplain);

        const boost::intrusive_ptr<ExpressionContext> expCtx;
        return uassertStatusOK(
            CanonicalQuery::canonicalize(operationContext(), std::move(qr.getValue())));
    }

    BSONObj runFindCommand(BSONObj findCmd) {
        const NamespaceString nss(CommandHelpers::parseNsCollectionRequired(kNss.db(), findCmd));

        auto cq = makeCanonicalQueryFromFindCommand(findCmd);
        std::vector<BSONObj> batch;
        auto cursorId = ClusterFind::runQuery(
            operationContext(), *cq, ReadPreferenceSetting(ReadPreference::PrimaryOnly), &batch);

        rpc::OpMsgReplyBuilder result;
        CursorResponseBuilder::Options options;
        options.isInitialResponse = true;
        CursorResponseBuilder firstBatch(&result, options);
        for (const auto& obj : batch) {
            firstBatch.append(obj);
        }
        firstBatch.done(cursorId, nss.ns());

        return result.releaseBody();
    }

    void runFindCommandSuccessful(BSONObj cmd, bool isTargeted) {
        auto future = launchAsync([&] {
            // Shouldn't throw.
            runFindCommand(cmd);
        });

        size_t numMocks = isTargeted ? 1 : numShards;
        for (size_t i = 0; i < numMocks; i++) {
            expectFindReturnsSuccess(i % numShards);
        }

        future.timed_get(kFutureTimeout);
    }

    void runFindCommandOneError(BSONObj cmd, ErrorCodes::Error code, bool isTargeted) {
        auto future = launchAsync([&] {
            // Shouldn't throw.
            runFindCommand(cmd);
        });

        size_t numMocks = isTargeted ? 1 : numShards;
        for (size_t i = 0; i < numMocks; i++) {
            expectFindReturnsError(code);
        }
        for (size_t i = 0; i < numMocks; i++) {
            expectFindReturnsSuccess(i % numShards);
        }

        future.timed_get(kFutureTimeout);
    }

    void runFindCommandMaxErrors(BSONObj cmd, ErrorCodes::Error code, bool isTargeted) {
        auto future =
            launchAsync([&] { ASSERT_THROWS_CODE(runFindCommand(cmd), DBException, code); });

        size_t numMocks =
            isTargeted ? ClusterFind::kMaxRetries : ClusterFind::kMaxRetries * numShards;
        for (size_t i = 0; i < numMocks; i++) {
            expectFindReturnsError(code);
        }

        future.timed_get(kFutureTimeout);
    }

    DbResponse runCommandThroughStrategy(BSONObj cmd) {
        // Create a new client/operation context per command
        auto client = getServiceContext()->makeClient("ClusterFindCmdClient");
        auto opCtx = client->makeOperationContext();

        const auto opMsgRequest = OpMsgRequest::fromDBAndBody(kNss.db(), cmd);

        return Strategy::clientCommand(opCtx.get(), opMsgRequest.serialize());
    }

    void runFindCommandInspectRequests(BSONObj cmd, InspectionCallback cb, bool isTargeted) {
        auto future = launchAsync([&] { runCommandThroughStrategy(cmd); });

        size_t numMocks = isTargeted ? 1 : numShards;
        for (size_t i = 0; i < numMocks; i++) {
            expectFindInspectRequest(i % numShards, cb);
        }

        future.timed_get(kFutureTimeout);
    }
};

TEST_F(ClusterFindTest, NoErrors) {
    loadRoutingTableWithTwoChunksAndTwoShards(kNss);

    // Target one shard.
    runFindCommandSuccessful(kFindCmdTargeted, true);

    // Target all shards.
    runFindCommandSuccessful(kFindCmdScatterGather, false);
}

// Verify ClusterFind::runQuery will retry on a snapshot error.
TEST_F(ClusterFindTest, RetryOnSnapshotError) {
    loadRoutingTableWithTwoChunksAndTwoShards(kNss);

    // Target one shard.
    runFindCommandOneError(kFindCmdTargeted, ErrorCodes::SnapshotUnavailable, true);
    runFindCommandOneError(kFindCmdTargeted, ErrorCodes::SnapshotTooOld, true);

    // Target all shards
    runFindCommandOneError(kFindCmdScatterGather, ErrorCodes::SnapshotUnavailable, false);
    runFindCommandOneError(kFindCmdScatterGather, ErrorCodes::SnapshotTooOld, false);
}

// Verify ClusterFind::runQuery will retry up to its max retry attempts on snapshot errors then
// return the final error it receives.
TEST_F(ClusterFindTest, MaxRetriesSnapshotErrors) {
    loadRoutingTableWithTwoChunksAndTwoShards(kNss);

    // Target one shard.
    runFindCommandMaxErrors(kFindCmdTargeted, ErrorCodes::SnapshotUnavailable, true);
    runFindCommandMaxErrors(kFindCmdTargeted, ErrorCodes::SnapshotTooOld, true);

    // Target all shards
    runFindCommandMaxErrors(kFindCmdScatterGather, ErrorCodes::SnapshotUnavailable, false);
    runFindCommandMaxErrors(kFindCmdScatterGather, ErrorCodes::SnapshotTooOld, false);
}

TEST_F(ClusterFindTest, AttachesAtClusterTimeForSnapshotReadConcern) {
    loadRoutingTableWithTwoChunksAndTwoShards(kNss);

    auto containsAtClusterTime = [](const executor::RemoteCommandRequest& request) {
        ASSERT(!request.cmdObj["readConcern"]["atClusterTime"].eoo());
    };

    // Target one shard.
    runFindCommandInspectRequests(
        appendSnapshotReadConcernAndTxnOptions(kFindCmdTargeted), containsAtClusterTime, true);

    // Target all shards.
    runFindCommandInspectRequests(appendSnapshotReadConcernAndTxnOptions(kFindCmdScatterGather),
                                  containsAtClusterTime,
                                  false);
}

TEST_F(ClusterFindTest, SnapshotReadConcernWithAfterClusterTime) {
    loadRoutingTableWithTwoChunksAndTwoShards(kNss);

    // This cannot be true in a real cluster, but is done to verify that the chosen atClusterTime
    // cannot be less than afterClusterTime.
    ASSERT_GT(kAfterClusterTime, kInMemoryLogicalTime);

    auto containsAtClusterTimeNoAfterClusterTime =
        [&](const executor::RemoteCommandRequest& request) {
            ASSERT(!request.cmdObj["readConcern"]["atClusterTime"].eoo());
            ASSERT(request.cmdObj["readConcern"]["afterClusterTime"].eoo());

            // The chosen atClusterTime should be greater than or equal to the request's
            // afterClusterTime.
            ASSERT_GTE(LogicalTime(request.cmdObj["readConcern"]["atClusterTime"].timestamp()),
                       kAfterClusterTime);
        };

    // Target one shard.
    runFindCommandInspectRequests(appendSnapshotReadConcernAndTxnOptions(kFindCmdTargeted, true),
                                  containsAtClusterTimeNoAfterClusterTime,
                                  true);

    // Target all shards.
    runFindCommandInspectRequests(
        appendSnapshotReadConcernAndTxnOptions(kFindCmdScatterGather, true),
        containsAtClusterTimeNoAfterClusterTime,
        false);
}

}  // namespace
}  // namespace mongo
