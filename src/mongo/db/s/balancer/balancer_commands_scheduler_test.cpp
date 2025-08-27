/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/s/balancer/balancer_commands_scheduler.h"
#include "mongo/db/s/balancer/balancer_commands_scheduler_impl.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/s/request_types/move_range_request_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace mongo {
namespace {


class BalancerCommandsSchedulerTest : public ConfigServerTestFixture {
public:
    const ShardId kShardId0 = ShardId("shard0");
    const ShardId kShardId1 = ShardId("shard1");
    const HostAndPort kShardHost0 = HostAndPort("TestHost0", 12345);
    const HostAndPort kShardHost1 = HostAndPort("TestHost1", 12346);

    const std::vector<ShardType> kShardList{
        ShardType(kShardId0.toString(), kShardHost0.toString()),
        ShardType(kShardId1.toString(), kShardHost1.toString())};

    const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("testDb.testColl");
    const NamespaceString kNssWithCustomizedSize =
        NamespaceString::createNamespaceString_forTest("testDb.testCollCustomized");

    const UUID kUuid = UUID::gen();

    static constexpr int64_t kDefaultMaxChunkSizeBytes = 128;
    static constexpr int64_t kCustomizedMaxChunkSizeBytes = 256;

    ChunkType makeChunk(long long min, const ShardId& shardId) {
        ChunkType chunk;
        chunk.setRange({BSON("x" << min), BSON("x" << min + 10)});
        chunk.setJumbo(false);
        chunk.setShard(shardId);
        chunk.setVersion(ChunkVersion({OID::gen(), Timestamp(10)}, {1, 1}));
        return chunk;
    }

    ShardsvrMoveRange makeMoveRangeRequest(long long min, const ShardId& to, const ShardId& from) {
        MoveRangeRequestBase base;
        base.setToShard(to);
        base.setMin(BSON("x" << min));
        base.setMax(BSON("x" << min + 10));

        ShardsvrMoveRange shardSvrRequest(kNss);
        shardSvrRequest.setDbName(DatabaseName::kAdmin);
        shardSvrRequest.setMoveRangeRequestBase(base);
        shardSvrRequest.setFromShard(from);
        shardSvrRequest.setCollectionTimestamp(Timestamp(10));
        shardSvrRequest.setMaxChunkSizeBytes(1024 * 1024);

        return shardSvrRequest;
    }


protected:
    void setUp() override {
        setUpAndInitializeConfigDb();
        setupShards(kShardList);
        // Scheduler commands target shards that need to be retrieved.
        auto opCtx = operationContext();
        configureTargeter(opCtx, kShardId0, kShardHost0);
        configureTargeter(opCtx, kShardId1, kShardHost1);
    }

    void tearDown() override {
        _scheduler.stop();
        ConfigServerTestFixture::tearDown();
    }

    /*
     * Extra setup function to define the whole sequence of (mocked) remote command responses that a
     * test is expected to receive during its execution.
     * - Must be invoked before running _scheduler.start()
     * - The returned future must be stored in a local variable and kept in scope to ensure that the
     * sequence gets generated
     */
    executor::NetworkTestEnv::FutureHandle<void> setRemoteResponses(
        std::vector<executor::NetworkTestEnv::OnCommandFunction> remoteResponseGenerators = {}) {
        std::vector<executor::NetworkTestEnv::OnCommandFunction> generatorsWithStartSequence;
        // Set an OK response for every shardSvrJoinMigration command send out by the start() method
        // of the commands scheduler
        for (size_t i = 0; i < kShardList.size(); ++i) {
            generatorsWithStartSequence.push_back(
                [&](const executor::RemoteCommandRequest& request) { return OkReply().toBSON(); });
        }
        generatorsWithStartSequence.insert(generatorsWithStartSequence.end(),
                                           remoteResponseGenerators.begin(),
                                           remoteResponseGenerators.end());
        return launchAsync([this, g = std::move(generatorsWithStartSequence)] { onCommands(g); });
    }

    void configureTargeter(OperationContext* opCtx, ShardId shardId, const HostAndPort& host) {
        auto targeter = RemoteCommandTargeterMock::get(
            uassertStatusOK(shardRegistry()->getShard(opCtx, shardId))->getTargeter());
        targeter->setFindHostReturnValue(host);
    }

    BalancerCommandsSchedulerImpl _scheduler;
};

TEST_F(BalancerCommandsSchedulerTest, StartAndStopScheduler) {
    auto remoteResponsesFuture = setRemoteResponses();
    _scheduler.start(operationContext());
    _scheduler.stop();
    remoteResponsesFuture.default_timed_get();
}

TEST_F(BalancerCommandsSchedulerTest, SuccessfulMoveRangeCommand) {
    auto remoteResponsesFuture =
        setRemoteResponses({[&](const executor::RemoteCommandRequest& request) {
            return OkReply().toBSON();
        }});
    _scheduler.start(operationContext());
    ShardsvrMoveRange shardsvrRequest(kNss);
    shardsvrRequest.setCollectionTimestamp(Timestamp(10));
    shardsvrRequest.setDbName(DatabaseName::kAdmin);
    shardsvrRequest.setFromShard(kShardId0);
    shardsvrRequest.setMaxChunkSizeBytes(1024);
    auto& moveRangeRequestBase = shardsvrRequest.getMoveRangeRequestBase();
    moveRangeRequestBase.setToShard(kShardId1);
    moveRangeRequestBase.setMin({});
    moveRangeRequestBase.setMax({});

    auto futureResponse = _scheduler.requestMoveRange(
        operationContext(), shardsvrRequest, WriteConcernOptions(), false /* issuedByRemoteUser */);
    ASSERT_OK(futureResponse.getNoThrow());
    remoteResponsesFuture.default_timed_get();
    _scheduler.stop();
}

TEST_F(BalancerCommandsSchedulerTest, SuccessfulMergeChunkCommand) {
    auto remoteResponsesFuture =
        setRemoteResponses({[&](const executor::RemoteCommandRequest& request) {
            return OkReply().toBSON();
        }});
    _scheduler.start(operationContext());

    ChunkRange range(BSON("x" << 0), BSON("x" << 20));
    ChunkVersion version({OID::gen(), Timestamp(10)}, {1, 1});
    auto futureResponse =
        _scheduler.requestMergeChunks(operationContext(), kNss, kShardId0, range, version);
    ASSERT_OK(futureResponse.getNoThrow());
    remoteResponsesFuture.default_timed_get();
    _scheduler.stop();
}

TEST_F(BalancerCommandsSchedulerTest, MergeChunkNonexistentShard) {
    auto remoteResponsesFuture = setRemoteResponses();
    _scheduler.start(operationContext());
    ChunkRange range(BSON("x" << 0), BSON("x" << 20));
    ChunkVersion version({OID::gen(), Timestamp(10)}, {1, 1});
    auto futureResponse = _scheduler.requestMergeChunks(
        operationContext(), kNss, ShardId("nonexistent"), range, version);
    auto shardNotFoundError = Status{ErrorCodes::ShardNotFound, "Shard nonexistent not found"};
    ASSERT_EQ(futureResponse.getNoThrow(), shardNotFoundError);
    remoteResponsesFuture.default_timed_get();
    _scheduler.stop();
}

TEST_F(BalancerCommandsSchedulerTest, SuccessfulRequestChunkDataSizeCommand) {
    BSONObjBuilder chunkSizeResponse;
    chunkSizeResponse.append("ok", "1");
    chunkSizeResponse.append("size", 156);
    chunkSizeResponse.append("numObjects", 25);
    auto remoteResponsesFuture =
        setRemoteResponses({[&](const executor::RemoteCommandRequest& request) {
            return chunkSizeResponse.obj();
        }});

    _scheduler.start(operationContext());
    ChunkType chunk = makeChunk(0, kShardId0);

    auto futureResponse =
        _scheduler.requestDataSize(operationContext(),
                                   kNss,
                                   chunk.getShard(),
                                   chunk.getRange(),
                                   ShardVersionFactory::make(chunk.getVersion()),
                                   KeyPattern(BSON("x" << 1)),
                                   false /* issuedByRemoteUser */,
                                   (kDefaultMaxChunkSizeBytes / 100) * 25 /* maxSize */);
    auto swReceivedDataSize = futureResponse.getNoThrow();
    ASSERT_OK(swReceivedDataSize.getStatus());
    auto receivedDataSize = swReceivedDataSize.getValue();
    ASSERT_EQ(receivedDataSize.sizeBytes, 156);
    ASSERT_EQ(receivedDataSize.numObjects, 25);
    remoteResponsesFuture.default_timed_get();
    _scheduler.stop();
}

TEST_F(BalancerCommandsSchedulerTest, SuccessfulMoveCollectionRequest) {

    ConfigServerTestFixture::setupDatabase(kNss.dbName(), kShardId1);

    auto remoteResponsesFuture = setRemoteResponses({[&](const executor::RemoteCommandRequest&
                                                             request) {
        // Expect to target the DBPrimary shard.
        ASSERT_EQ(request.target, kShardHost1);

        // Expect to get the correct nss.
        ASSERT_EQ(
            kNss.toString_forTest(),
            request.cmdObj.getStringField(ShardsvrReshardCollection::kCommandParameterFieldName));

        // Expect to get 1 num initial chunks.
        ASSERT(request.cmdObj.hasField(ShardsvrReshardCollection::kNumInitialChunksFieldName));
        ASSERT_EQ(
            1, request.cmdObj.getIntField(ShardsvrReshardCollection::kNumInitialChunksFieldName));

        // Expect to get the proper shard as a destination for the collection.
        ASSERT(request.cmdObj.hasField(ShardsvrReshardCollection::kShardDistributionFieldName));
        const auto shardDistributionArray =
            request.cmdObj.getField(ShardsvrReshardCollection::kShardDistributionFieldName).Array();
        ASSERT_EQ(1, shardDistributionArray.size());

        const auto shardKeyRange = ShardKeyRange::parse(
            shardDistributionArray.at(0).Obj(), IDLParserContext("BalancerCommandsSchedulerTest"));
        ASSERT_EQ(kShardId0, shardKeyRange.getShard());

        ASSERT_EQ(
            ReshardingProvenance_serializer(ReshardingProvenanceEnum::kBalancerMoveCollection),
            request.cmdObj.getStringField(ShardsvrReshardCollection::kProvenanceFieldName));

        return OkReply().toBSON();
    }});
    _scheduler.start(operationContext());

    auto catalogClient = ShardingCatalogManager::get(operationContext())->localCatalogClient();
    const auto dbEntry = catalogClient->getDatabase(
        operationContext(), kNss.dbName(), repl::ReadConcernLevel::kMajorityReadConcern);
    auto futureResponse = _scheduler.requestMoveCollection(
        operationContext(), kNss, kShardId0, kShardId1, dbEntry.getVersion());
    ASSERT_OK(futureResponse.getNoThrow());
    remoteResponsesFuture.default_timed_get();
    _scheduler.stop();
}

TEST_F(BalancerCommandsSchedulerTest, CommandFailsWhenNetworkReturnsError) {
    auto timeoutError = Status{ErrorCodes::NetworkTimeout, "Mock error: network timed out"};
    auto remoteResponsesFuture =
        setRemoteResponses({[&](const executor::RemoteCommandRequest& request) {
            return timeoutError;
        }});
    _scheduler.start(operationContext());
    auto req = makeMoveRangeRequest(0, kShardId1, kShardId0);
    auto futureResponse = _scheduler.requestMoveRange(
        operationContext(), req, WriteConcernOptions(), false /* issuedByRemoteUser */);
    ASSERT_EQUALS(futureResponse.getNoThrow(), timeoutError);
    remoteResponsesFuture.default_timed_get();
    _scheduler.stop();
}

TEST_F(BalancerCommandsSchedulerTest, CommandFailsWhenSchedulerIsStopped) {
    auto req = makeMoveRangeRequest(0, kShardId1, kShardId0);
    auto futureResponse = _scheduler.requestMoveRange(
        operationContext(), req, WriteConcernOptions(), false /* issuedByRemoteUser */);
    ASSERT_EQUALS(futureResponse.getNoThrow(),
                  Status(ErrorCodes::BalancerInterrupted,
                         "Request rejected - balancer scheduler is stopped"));
}

TEST_F(BalancerCommandsSchedulerTest, CommandCanceledIfUnsubmittedBeforeBalancerStops) {
    SemiFuture<void> futureResponse;
    {
        auto remoteResponsesFuture = setRemoteResponses();
        FailPointEnableBlock failPoint("pauseSubmissionsFailPoint");
        _scheduler.start(operationContext());
        auto req = makeMoveRangeRequest(0, kShardId1, kShardId0);

        futureResponse = _scheduler.requestMoveRange(
            operationContext(), req, WriteConcernOptions(), false /* issuedByRemoteUser */);
        _scheduler.stop();
        remoteResponsesFuture.default_timed_get();
    }
    ASSERT_EQUALS(futureResponse.getNoThrow(),
                  Status(ErrorCodes::BalancerInterrupted,
                         "Request cancelled - balancer scheduler is stopping"));
}

}  // namespace
}  // namespace mongo
