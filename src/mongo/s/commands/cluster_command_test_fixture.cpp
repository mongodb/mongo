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


#include <boost/move/utility_core.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include <system_error>
#include <utility>

#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/keys_collection_client_sharded.h"
#include "mongo/db/keys_collection_manager.h"
#include "mongo/db/keys_collection_manager_gen.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/request_execution_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/vector_clock.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/commands/cluster_command_test_fixture.h"
#include "mongo/s/commands/strategy.h"
#include "mongo/s/grid.h"
#include "mongo/transport/service_executor.h"
#include "mongo/unittest/assert.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/tick_source_mock.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {

void ClusterCommandTestFixture::setUp() {
    RouterCatalogCacheTestFixture::setUp();
    RouterCatalogCacheTestFixture::setupNShards(numShards);

    Grid::get(getServiceContext())->setShardingInitialized();

    // Set the initial clusterTime.
    VectorClock::get(getServiceContext())->advanceClusterTime_forTest(kInMemoryLogicalTime);

    auto keysCollectionClient = std::make_unique<KeysCollectionClientSharded>(
        Grid::get(operationContext())->catalogClient());

    auto keyManager = std::make_shared<KeysCollectionManager>(
        "dummy", std::move(keysCollectionClient), Seconds(KeysRotationIntervalSec));

    auto validator = std::make_unique<LogicalTimeValidator>(keyManager);
    LogicalTimeValidator::set(getServiceContext(), std::move(validator));

    LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());

    // Set up a tick source for transaction metrics.
    auto tickSource = std::make_unique<TickSourceMock<Microseconds>>();
    tickSource->reset(1);
    getServiceContext()->setTickSource(std::move(tickSource));

    loadRoutingTableWithTwoChunksAndTwoShards(kNss);

    _staleVersionAndSnapshotRetriesBlock = std::make_unique<FailPointEnableBlock>(
        "enableStaleVersionAndSnapshotRetriesWithinTransactions");

    // The ReadWriteConcernDefaults decoration on the service context won't always be created,
    // so we should manually instantiate it to ensure it exists in our tests.
    ReadWriteConcernDefaults::create(getServiceContext(), _lookupMock.getFetchDefaultsFn());
}

BSONObj ClusterCommandTestFixture::_makeCmd(BSONObj cmdObj, bool includeAfterClusterTime) {
    BSONObjBuilder bob(cmdObj);
    // Each command runs in a new session.
    bob.append("lsid", makeLogicalSessionIdForTest().toBSON());
    bob.append("txnNumber", TxnNumber(1));
    bob.append("autocommit", false);
    bob.append("startTransaction", true);

    BSONObjBuilder readConcernBob = bob.subobjStart(repl::ReadConcernArgs::kReadConcernFieldName);
    readConcernBob.append("level", "snapshot");
    if (includeAfterClusterTime) {
        readConcernBob.append("afterClusterTime", kAfterClusterTime);
    }

    readConcernBob.doneFast();
    return bob.obj();
}

void ClusterCommandTestFixture::expectReturnsError(ErrorCodes::Error code) {
    onCommandForPoolExecutor([code](const executor::RemoteCommandRequest& request) {
        BSONObjBuilder resBob;
        CommandHelpers::appendCommandStatusNoThrow(resBob, Status(code, "dummy error"));
        return resBob.obj();
    });
}

DbResponse ClusterCommandTestFixture::runCommand(BSONObj cmd) {
    // Create a new client/operation context per command
    auto client = getServiceContext()->getService()->makeClient("ClusterCmdClient");
    auto opCtx = client->makeOperationContext();

    {
        // Have the new client use the dedicated threading model. This ensures the synchronous
        // execution of the command by the client thread.
        stdx::lock_guard lk(*client.get());
        auto seCtx = std::make_unique<transport::ServiceExecutorContext>();
        seCtx->setThreadModel(seCtx->kSynchronous);
        transport::ServiceExecutorContext::set(client.get(), std::move(seCtx));
    }

    OpMsgRequest opMsgRequest;

    // If bulkWrite then append adminDB.
    if (cmd.firstElementFieldNameStringData() == "bulkWrite") {
        opMsgRequest = OpMsgRequestBuilder::createWithValidatedTenancyScope(
            DatabaseName::kAdmin, auth::ValidatedTenancyScope::kNotRequired, cmd);
    } else {
        opMsgRequest = OpMsgRequestBuilder::createWithValidatedTenancyScope(
            kNss.dbName(), auth::ValidatedTenancyScope::get(opCtx.get()), cmd);
    }

    AlternativeClientRegion acr(client);
    auto rec = std::make_shared<RequestExecutionContext>(opCtx.get(), opMsgRequest.serialize());
    return Strategy::clientCommand(std::move(rec)).get();
}

DbResponse ClusterCommandTestFixture::runCommandSuccessful(BSONObj cmd, bool isTargeted) {
    auto future = launchAsync([&] {
        // Shouldn't throw.
        return runCommand(cmd);
    });

    size_t numMocks = isTargeted ? 1 : numShards;
    for (size_t i = 0; i < numMocks; i++) {
        expectReturnsSuccess(i % numShards);
    }

    return future.default_timed_get();
}

void ClusterCommandTestFixture::runTxnCommandOneError(BSONObj cmd,
                                                      ErrorCodes::Error code,
                                                      bool isTargeted) {
    auto future = launchAsync([&] {
        // Shouldn't throw.
        runCommand(cmd);
    });

    size_t numMocks = isTargeted ? 1 : numShards;
    for (size_t i = 0; i < numMocks; i++) {
        expectReturnsError(code);
    }

    // In a transaction, when the router encounters a retryable error it sends abortTransaction to
    // each pending participant shard before retrying.
    for (size_t i = 0; i < numMocks; i++) {
        expectAbortTransaction();
    }

    for (size_t i = 0; i < numMocks; i++) {
        expectReturnsSuccess(i % numShards);
    }

    future.default_timed_get();
}

void ClusterCommandTestFixture::runCommandInspectRequests(BSONObj cmd,
                                                          InspectionCallback cb,
                                                          bool isTargeted) {
    auto future = launchAsync([&] { runCommand(cmd); });

    size_t numMocks = isTargeted ? 1 : numShards;
    for (size_t i = 0; i < numMocks; i++) {
        expectInspectRequest(i % numShards, cb);
    }

    future.default_timed_get();
}

void ClusterCommandTestFixture::expectAbortTransaction() {
    onCommandForPoolExecutor([this](const executor::RemoteCommandRequest& request) {
        auto cmdName = request.cmdObj.firstElement().fieldNameStringData();
        ASSERT_EQ(cmdName, "abortTransaction");

        BSONObjBuilder bob;
        bob.append("ok", 1);
        appendTxnResponseMetadata(bob);
        return bob.obj();
    });
}

void ClusterCommandTestFixture::runTxnCommandMaxErrors(BSONObj cmd,
                                                       ErrorCodes::Error code,
                                                       bool isTargeted) {
    auto future = launchAsync([&] { runCommand(cmd); });

    size_t numTargetedShards = isTargeted ? 1 : numShards;
    for (size_t i = 0; i < kMaxNumStaleVersionRetries - 1; i++) {
        for (size_t j = 0; j < numTargetedShards; j++) {
            expectReturnsError(code);
        }

        // In a transaction, when the router encounters a retryable error it sends abortTransaction
        // to each pending participant shard before retrying.
        for (size_t j = 0; j < numTargetedShards; j++) {
            expectAbortTransaction();
        }
    }

    // The router should exhaust its retries here.
    for (size_t j = 0; j < numTargetedShards; j++) {
        expectReturnsError(code);
    }

    // In a transaction, each targeted shard is sent abortTransaction when the router exhausts its
    // retries.
    for (size_t i = 0; i < numTargetedShards; i++) {
        expectAbortTransaction();
    }

    future.default_timed_get();
}

void ClusterCommandTestFixture::testNoErrors(BSONObj targetedCmd, BSONObj scatterGatherCmd) {

    // Target one shard.
    runCommandSuccessful(_makeCmd(targetedCmd), true);

    // Target all shards.
    if (!scatterGatherCmd.isEmpty()) {
        runCommandSuccessful(_makeCmd(scatterGatherCmd), false);
    }
}

void ClusterCommandTestFixture::testRetryOnSnapshotError(BSONObj targetedCmd,
                                                         BSONObj scatterGatherCmd) {
    // Target one shard.
    runTxnCommandOneError(_makeCmd(targetedCmd), ErrorCodes::SnapshotUnavailable, true);
    runTxnCommandOneError(_makeCmd(targetedCmd), ErrorCodes::SnapshotTooOld, true);

    // Target all shards
    if (!scatterGatherCmd.isEmpty()) {
        runTxnCommandOneError(_makeCmd(scatterGatherCmd), ErrorCodes::SnapshotUnavailable, false);
        runTxnCommandOneError(_makeCmd(scatterGatherCmd), ErrorCodes::SnapshotTooOld, false);
    }
}

void ClusterCommandTestFixture::testMaxRetriesSnapshotErrors(BSONObj targetedCmd,
                                                             BSONObj scatterGatherCmd) {
    // Target one shard.
    runTxnCommandMaxErrors(_makeCmd(targetedCmd), ErrorCodes::SnapshotUnavailable, true);
    runTxnCommandMaxErrors(_makeCmd(targetedCmd), ErrorCodes::SnapshotTooOld, true);

    // Target all shards
    if (!scatterGatherCmd.isEmpty()) {
        runTxnCommandMaxErrors(_makeCmd(scatterGatherCmd), ErrorCodes::SnapshotUnavailable, false);
        runTxnCommandMaxErrors(_makeCmd(scatterGatherCmd), ErrorCodes::SnapshotTooOld, false);
    }
}

void ClusterCommandTestFixture::testAttachesAtClusterTimeForSnapshotReadConcern(
    BSONObj targetedCmd, BSONObj scatterGatherCmd) {

    auto containsAtClusterTime = [](const executor::RemoteCommandRequest& request) {
        ASSERT(!request.cmdObj["readConcern"]["atClusterTime"].eoo());
    };

    // Target one shard.
    runCommandInspectRequests(_makeCmd(targetedCmd), containsAtClusterTime, true);

    // Target all shards.
    if (!scatterGatherCmd.isEmpty()) {
        runCommandInspectRequests(_makeCmd(scatterGatherCmd), containsAtClusterTime, false);
    }
}

void ClusterCommandTestFixture::testSnapshotReadConcernWithAfterClusterTime(
    BSONObj targetedCmd, BSONObj scatterGatherCmd) {

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
    runCommandInspectRequests(
        _makeCmd(targetedCmd, true), containsAtClusterTimeNoAfterClusterTime, true);

    // Target all shards.
    if (!scatterGatherCmd.isEmpty()) {
        runCommandInspectRequests(
            _makeCmd(scatterGatherCmd, true), containsAtClusterTimeNoAfterClusterTime, false);
    }
}

void ClusterCommandTestFixture::testIncludeQueryStatsMetrics(BSONObj cmd, bool isTargeted) {
    const std::string fieldName = "includeQueryStatsMetrics";

    // The given command should not set includeQueryStatsMetrics.
    ASSERT(cmd[fieldName].eoo());

    BSONObj cmdIncludeTrue = cmd.addField(BSON(fieldName << true).firstElement());
    BSONObj cmdIncludeFalse = cmd.addField(BSON(fieldName << false).firstElement());

    auto expectFieldIs = [&](bool value) {
        return [value, &fieldName](const executor::RemoteCommandRequest& request) {
            auto elt = request.cmdObj[fieldName];
            ASSERT(!elt.eoo());
            ASSERT_EQ(elt.boolean(), value);
        };
    };

    auto expectNoField = [&](const executor::RemoteCommandRequest& request) {
        ASSERT(request.cmdObj[fieldName].eoo());
    };

    {
        RAIIServerParameterControllerForTest flag("featureFlagQueryStatsDataBearingNodes", true);

        {
            // No rate limit i.e., no requests are rate limited and each one is allowed to gather
            // stats. We'll always request metrics, even if the user set includeQueryStatsMetrics
            // to false.
            RAIIServerParameterControllerForTest rateLimit("internalQueryStatsRateLimit", -1);

            runCommandInspectRequests(cmd, expectFieldIs(true), isTargeted);
            runCommandInspectRequests(cmdIncludeTrue, expectFieldIs(true), isTargeted);
            runCommandInspectRequests(cmdIncludeFalse, expectFieldIs(true), isTargeted);
        }

        {
            // Rate limit is 0 i.e., every request is rate-limited.
            RAIIServerParameterControllerForTest rateLimit("internalQueryStatsRateLimit", 0);

            // If the user doesn't give includeQueryStatsMetrics, we won't insert the field.
            runCommandInspectRequests(cmd, expectNoField, isTargeted);

            // If the user passed us includeQueryStatsMetrics, we'll pass it through.
            runCommandInspectRequests(cmdIncludeTrue, expectFieldIs(true), isTargeted);
            runCommandInspectRequests(cmdIncludeFalse, expectFieldIs(false), isTargeted);
        }
    }

    {
        RAIIServerParameterControllerForTest flag("featureFlagQueryStatsDataBearingNodes", false);
        RAIIServerParameterControllerForTest rateLimit("internalQueryStatsRateLimit", -1);

        // Having the feature flag disabled means we won't set the field unrequested.
        runCommandInspectRequests(cmd, expectNoField, isTargeted);

        // We will still pass through the field when the feature flag is false.
        runCommandInspectRequests(cmdIncludeTrue, expectFieldIs(true), isTargeted);
        runCommandInspectRequests(cmdIncludeFalse, expectFieldIs(false), isTargeted);
    }
}

void ClusterCommandTestFixture::appendTxnResponseMetadata(BSONObjBuilder& bob) {
    // Set readOnly to false to avoid opting in to the read-only optimization.
    bob.append(TxnResponseMetadata::kReadOnlyFieldName, false);
}

// Satisfies dependency from StoreSASLOPtions.
MONGO_STARTUP_OPTIONS_STORE(CoreOptions)(InitializerContext*) {}

}  // namespace mongo
