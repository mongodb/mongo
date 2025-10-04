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

#include "mongo/db/s/resharding/resharding_coordinator_service_external_state.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/s/resharding/recipient_resume_document_gen.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/executor/mock_async_rpc.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/future_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace resharding {
namespace {

class ReshardingCoordinatorServiceExternalStateTest : service_context_test::WithSetupTransportLayer,
                                                      public ConfigServerTestFixture {
public:
    void setUp() override {
        ConfigServerTestFixture::setUp();

        ShardType shard0;
        shard0.setName(shardId0.toString());
        shard0.setHost(shardId0.toString() + ":1234");
        ShardType shard1;
        shard1.setName(shardId1.toString());
        shard1.setHost(shardId1.toString() + ":1234");
        ShardType shard2;
        shard2.setName(shardId2.toString());
        shard2.setHost(shardId2.toString() + ":1234");
        std::vector<ShardType> shards{shard0, shard1, shard2};

        setupShards(shards);
        for (const auto& shard : shards) {
            auto hostAndPort = HostAndPort(shard.getHost());
            auto connectionString = ConnectionString(hostAndPort);
            targeterFactory()->addTargeterToReturn(
                connectionString, [hostAndPort, connectionString] {
                    std::unique_ptr<RemoteCommandTargeterMock> targeter(
                        std::make_unique<RemoteCommandTargeterMock>());
                    targeter->setConnectionStringReturnValue(connectionString);
                    targeter->setFindHostReturnValue(hostAndPort);
                    return targeter;
                }());
        }

        // Set up the task executor.
        ThreadPool::Options threadPoolOptions;
        threadPoolOptions.poolName = "ReshardingCoordinatorExternalStateTest";

        executor = executor::ThreadPoolTaskExecutor::create(
            std::make_unique<ThreadPool>(threadPoolOptions),
            executor::makeNetworkInterface("ReshardingCoordinatorExternalStateTest"));
        executor->startup();
        taskExecutor = std::make_shared<executor::ScopedTaskExecutor>(executor);

        // Set up the async RPC mock.
        auto asyncRPCMock = std::make_unique<async_rpc::AsyncMockAsyncRPCRunner>();
        async_rpc::detail::AsyncRPCRunner::set(getServiceContext(), std::move(asyncRPCMock));
    }

    void tearDown() override {
        ConfigServerTestFixture::tearDown();
        executor->shutdown();
    }

    // This is a map from each donor shard id to the number of documents to copy from that donor
    // shard (if that info is available).
    using DocumentsToCopyMap = std::map<ShardId, boost::optional<int64_t>>;
    // This is a map from each donor shard id to the number of documents a recipient shard copied
    // from that donor shard (if that info is available).
    using RecipientDocumentsCopiedMap = std::map<ShardId, boost::optional<int64_t>>;
    using DocumentsCopiedMap = std::map<ShardId, RecipientDocumentsCopiedMap>;
    // This is a map from each donor or recipient shard id to the final number of documents that
    // shard (if that info is available).
    using DocumentsFinalMap = std::map<ShardId, boost::optional<int64_t>>;

    ReshardingCoordinatorDocument makeCoordinatorDocument(const DocumentsToCopyMap& docsToCopy,
                                                          const DocumentsCopiedMap& docsCopied) {
        std::vector<DonorShardEntry> donorShards;
        for (auto&& [donorShardId, donorDocsToCopy] : docsToCopy) {
            DonorShardEntry donorEntry(donorShardId, {});
            donorEntry.setDocumentsToCopy(donorDocsToCopy);
            donorShards.emplace_back(donorEntry);
        }

        std::vector<RecipientShardEntry> recipientShards;
        for (auto&& [recipientShardId, _] : docsCopied) {
            RecipientShardEntry recipientEntry(recipientShardId, {});
            recipientShards.emplace_back(recipientEntry);
        }

        ReshardingCoordinatorDocument coordinatorDoc;
        coordinatorDoc.setCommonReshardingMetadata(
            {reshardingUUID, sourceNss, sourceUUID, tempNss, shardKey});
        coordinatorDoc.setState(CoordinatorStateEnum::kCloning);
        coordinatorDoc.setDonorShards(donorShards);
        coordinatorDoc.setRecipientShards(recipientShards);

        return coordinatorDoc;
    }

    ReshardingCoordinatorDocument makeCoordinatorDocument(
        const DocumentsFinalMap& docsFinalOnDonors,
        const DocumentsFinalMap& docsFinalOnRecipients) {
        std::vector<DonorShardEntry> donorShards;
        for (auto&& [donorShardId, donorDocsFinal] : docsFinalOnDonors) {
            DonorShardEntry donorEntry(donorShardId, {});
            donorEntry.setDocumentsFinal(donorDocsFinal);
            donorShards.emplace_back(donorEntry);
        }

        std::vector<RecipientShardEntry> recipientShards;
        for (auto&& [recipientShardId, recipientDocsFinal] : docsFinalOnRecipients) {
            RecipientShardContext mutableState;
            mutableState.setTotalNumDocuments(recipientDocsFinal);
            RecipientShardEntry recipientEntry(recipientShardId, mutableState);
            recipientShards.emplace_back(recipientEntry);
        }

        ReshardingCoordinatorDocument coordinatorDoc;
        coordinatorDoc.setCommonReshardingMetadata(
            {reshardingUUID, sourceNss, sourceUUID, tempNss, shardKey});
        coordinatorDoc.setState(CoordinatorStateEnum::kApplying);
        coordinatorDoc.setDonorShards(donorShards);
        coordinatorDoc.setRecipientShards(recipientShards);

        return coordinatorDoc;
    }

    std::vector<BSONObj> makeRecipientCloningMetricsAggregateDocuments(
        const UUID& reshardingUUID, const RecipientDocumentsCopiedMap& docsCopied) {
        BSONObjBuilder bob;

        for (auto donorIter = docsCopied.begin(); donorIter != docsCopied.end(); ++donorIter) {
            auto donorShardId = donorIter->first;
            auto donorDocsCopied = donorIter->second;
            if (donorDocsCopied) {
                bob.append(donorShardId.toString(), *donorDocsCopied);
            } else {
                bob.appendNull(donorShardId.toString());
            }
        }

        return {BSON("documentsCopied" << bob.obj())};
    }

    auto mockRecipientCloningMetricsResponses(const UUID& reshardingUUID,
                                              const DocumentsCopiedMap& docsCopied) {
        std::vector<Future<void>> expectations;

        for (auto recipientIter = docsCopied.begin(); recipientIter != docsCopied.end();
             ++recipientIter) {
            auto recipientShardId = recipientIter->first;
            auto recipientDocsCopied = recipientIter->second;

            auto asyncRPCRunner = dynamic_cast<async_rpc::AsyncMockAsyncRPCRunner*>(
                async_rpc::detail::AsyncRPCRunner::get(operationContext()->getServiceContext()));

            auto matcher = [&reshardingUUID, recipientShardId = recipientShardId](
                               const async_rpc::AsyncMockAsyncRPCRunner::Request& req) {
                ShardId shardId{req.target.host()};

                if (shardId != recipientShardId) {
                    return false;
                }

                auto aggRequest = AggregateCommandRequest::parse(
                    req.cmdBSON.addFields(BSON("$db" << req.dbName)),
                    IDLParserContext("mockRecipientCloningMetricsResponses"));

                ASSERT_EQ(aggRequest.getNamespace(),
                          NamespaceString::kRecipientReshardingResumeDataNamespace);

                auto pipeline = aggRequest.getPipeline();
                ASSERT_EQ(pipeline.size(), 3);
                ASSERT_BSONOBJ_EQ(
                    pipeline[0],
                    BSON(
                        "$match" << BSON((ReshardingRecipientResumeData::kIdFieldName + "." +
                                          ReshardingRecipientResumeDataId::kReshardingUUIDFieldName)
                                         << reshardingUUID)));
                ASSERT_BSONOBJ_EQ(
                    pipeline[1],
                    BSON("$group" << BSON(
                             "_id"
                             << BSONNULL << "pairs"
                             << BSON("$push" << BSON(
                                         "k" << ("$" + ReshardingRecipientResumeData::kIdFieldName +
                                                 "." +
                                                 ReshardingRecipientResumeDataId::kShardIdFieldName)
                                             << "v"
                                             << ("$" +
                                                 ReshardingRecipientResumeData::
                                                     kDocumentsCopiedFieldName))))));
                ASSERT_BSONOBJ_EQ(
                    pipeline[2],
                    BSON("$project" << BSON("_id" << 0 << "documentsCopied"
                                                  << BSON("$arrayToObject" << "$pairs"))));

                ASSERT_BSONOBJ_EQ(aggRequest.getReadConcern()->toBSON(),
                                  repl::ReadConcernArgs::kMajority.toBSON());
                ASSERT_BSONOBJ_EQ(
                    *aggRequest.getUnwrappedReadPref(),
                    BSON("$readPreference"
                         << ReadPreferenceSetting{ReadPreference::PrimaryOnly}.toInnerBSON()));

                return true;
            };

            std::vector<BSONObj> docs =
                makeRecipientCloningMetricsAggregateDocuments(reshardingUUID, recipientDocsCopied);
            CursorResponse cursorResponse(
                NamespaceString::kRecipientReshardingResumeDataNamespace, 0 /* cursorId */, docs);
            auto response = cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);

            expectations.push_back(
                asyncRPCRunner
                    ->expect(matcher, std::move(response), "mockRecipientCloningMetricsResponses")
                    .unsafeToInlineFuture());
        }

        return whenAllSucceed(std::move(expectations));
    }

    auto mockDonorCloningMetricsResponses(
        const std::map<ShardId, std::vector<BSONObj>>& responseDocs) {
        std::vector<Future<void>> expectations;

        for (auto donorIter = responseDocs.begin(); donorIter != responseDocs.end(); ++donorIter) {
            auto donorShardId = donorIter->first;
            auto donorResponseDocs = donorIter->second;

            auto asyncRPCRunner = dynamic_cast<async_rpc::AsyncMockAsyncRPCRunner*>(
                async_rpc::detail::AsyncRPCRunner::get(operationContext()->getServiceContext()));

            auto matcher = [this, donorShardId = donorShardId](
                               const async_rpc::AsyncMockAsyncRPCRunner::Request& req) {
                ShardId shardId{req.target.host()};

                if (shardId != donorShardId) {
                    return false;
                }

                auto aggRequest = AggregateCommandRequest::parse(
                    req.cmdBSON.addFields(BSON("$db" << req.dbName)),
                    IDLParserContext("mockRecipientCloningMetricsResponses"));

                ASSERT_EQUALS(aggRequest.getNamespace(), sourceNss);

                ASSERT_EQ(aggRequest.getPipeline().size(), 1);
                ASSERT_BSONOBJ_EQ(aggRequest.getPipeline()[0], BSON("$count" << "count"));
                ASSERT_BSONOBJ_EQ(*aggRequest.getHint(), BSON("_id" << 1));

                ASSERT_BSONOBJ_EQ(
                    aggRequest.getReadConcern()->toBSON(),
                    BSON("readConcern" << BSON("level" << "snapshot"
                                                       << "atClusterTime" << cloneTimestamp)));
                ASSERT_BSONOBJ_EQ(*aggRequest.getUnwrappedReadPref(),
                                  BSON("$readPreference" << BSON("mode" << "secondaryPreferred")));
                ASSERT_EQ(aggRequest.getShardVersion(), shardVersions.find(donorShardId)->second);

                return true;
            };

            CursorResponse cursorResponse(sourceNss, 0 /* cursorId */, donorResponseDocs);
            auto response = cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);

            expectations.push_back(
                asyncRPCRunner
                    ->expect(matcher, std::move(response), "mockDonorCloningMetricsResponses")
                    .unsafeToInlineFuture());
        }

        return whenAllSucceed(std::move(expectations));
    }

    auto mockDonorDeltaMetricsResponses(const std::map<ShardId, int64_t>& docsDelta) {
        std::vector<Future<void>> expectations;

        for (auto donorIter = docsDelta.begin(); donorIter != docsDelta.end(); ++donorIter) {
            auto donorShardId = donorIter->first;
            auto donorDocsDelta = donorIter->second;

            auto asyncRPCRunner = dynamic_cast<async_rpc::AsyncMockAsyncRPCRunner*>(
                async_rpc::detail::AsyncRPCRunner::get(operationContext()->getServiceContext()));

            auto matcher = [this, donorShardId = donorShardId](
                               const async_rpc::AsyncMockAsyncRPCRunner::Request& req) {
                ShardId shardId{req.target.host()};

                if (shardId != donorShardId) {
                    return false;
                }

                auto fetchCmd = ShardsvrReshardingDonorFetchFinalCollectionStats::parse(
                    req.cmdBSON.addFields(BSON("$db" << req.dbName)),
                    IDLParserContext("mockDonorDeltaMetricsResponses"));

                ASSERT_EQ(fetchCmd.getCommandParameter(), sourceNss);
                ASSERT_EQ(fetchCmd.getReshardingUUID(), reshardingUUID);
                return true;
            };

            ShardsvrReshardingDonorFetchFinalCollectionStatsResponse response(donorDocsDelta);
            expectations.push_back(asyncRPCRunner
                                       ->expect(matcher,
                                                response.toBSON().addFields(BSON("ok" << 1)),
                                                "mockDonorDeltaMetricsResponses")
                                       .unsafeToInlineFuture());
        }

        return whenAllSucceed(std::move(expectations));
    }


    auto getTaskExecutor() {
        return **taskExecutor;
    }

    auto getCancellationToken() {
        return operationContext()->getCancellationToken();
    }

protected:
    const ShardId shardId0{"shard0"};
    const ShardId shardId1{"shard1"};
    const ShardId shardId2{"shard2"};

    const UUID reshardingUUID = UUID::gen();
    const NamespaceString sourceNss =
        NamespaceString::createNamespaceString_forTest("testDb", "testColl");
    const UUID sourceUUID = UUID::gen();
    const NamespaceString tempNss =
        resharding::constructTemporaryReshardingNss(sourceNss, sourceUUID);
    const BSONObj shardKey = BSON("skey" << 1);
    const Timestamp cloneTimestamp = Timestamp(220, 220);

    const ShardVersion shardVersion0 = ShardVersionFactory::make(ChunkVersion(
        CollectionGeneration{OID::gen(), Timestamp(224, 224)}, CollectionPlacement(10, 1)));
    const ShardVersion shardVersion1 = ShardVersionFactory::make(ChunkVersion(
        CollectionGeneration{OID::gen(), Timestamp(224, 225)}, CollectionPlacement(10, 2)));

    const std::map<ShardId, ShardVersion> shardVersions{{shardId0, shardVersion0},
                                                        {shardId1, shardVersion1}};

    std::shared_ptr<executor::ScopedTaskExecutor> taskExecutor;
    std::shared_ptr<executor::ThreadPoolTaskExecutor> executor;
};

TEST_F(ReshardingCoordinatorServiceExternalStateTest, VerifyClonedCollectionSuccess_Basic) {
    DocumentsToCopyMap docsToCopy{
        {shardId0, 10},
    };
    DocumentsCopiedMap docsCopied{
        {shardId0, {{shardId0, 10}}},
    };

    auto coordinatorDoc = makeCoordinatorDocument(docsToCopy, docsCopied);
    auto future =
        mockRecipientCloningMetricsResponses(coordinatorDoc.getReshardingUUID(), docsCopied);

    ReshardingCoordinatorExternalStateImpl externalState;
    externalState.verifyClonedCollection(
        operationContext(), getTaskExecutor(), getCancellationToken(), coordinatorDoc);
    future.get();
}

TEST_F(ReshardingCoordinatorServiceExternalStateTest,
       VerifyClonedCollectionSuccess_NoCloningOnSubsetOfRecipientShards) {
    DocumentsToCopyMap docsToCopy{
        {shardId0, 10},
    };
    DocumentsCopiedMap docsCopied{
        // shard0 did not copy documents from any donor shard.
        {shardId0, {}},
        {shardId1, {{shardId0, 10}}},
    };

    auto coordinatorDoc = makeCoordinatorDocument(docsToCopy, docsCopied);
    auto future =
        mockRecipientCloningMetricsResponses(coordinatorDoc.getReshardingUUID(), docsCopied);

    ReshardingCoordinatorExternalStateImpl externalState;
    externalState.verifyClonedCollection(
        operationContext(), getTaskExecutor(), getCancellationToken(), coordinatorDoc);
    future.get();
}

TEST_F(ReshardingCoordinatorServiceExternalStateTest,
       VerifyClonedCollectionSuccess_NoCloningFromSubsetOfDonorShards) {
    DocumentsToCopyMap docsToCopy{
        {shardId0, 10},
        {shardId1, 20},
    };
    DocumentsCopiedMap docsCopied{
        // shard0 only copied documents from one of the donor shards.
        {shardId1, {{shardId1, 15}}},
        {shardId2, {{shardId0, 10}, {shardId1, 5}}},
    };

    auto coordinatorDoc = makeCoordinatorDocument(docsToCopy, docsCopied);
    auto future =
        mockRecipientCloningMetricsResponses(coordinatorDoc.getReshardingUUID(), docsCopied);

    ReshardingCoordinatorExternalStateImpl externalState;
    externalState.verifyClonedCollection(
        operationContext(), getTaskExecutor(), getCancellationToken(), coordinatorDoc);
    future.get();
}

TEST_F(ReshardingCoordinatorServiceExternalStateTest, VerifyClonedCollectionSuccess_Mixed) {
    DocumentsToCopyMap docsToCopy{
        {shardId0, 10},
        {shardId1, 20},
    };
    DocumentsCopiedMap docsCopied{
        // shard0 did not copy documents from any donor shard.
        {shardId0, {}},
        // shard1 copied documents from both donor shards.
        {shardId1, {{shardId0, 10}, {shardId1, 15}}},
        // shard2 only copied documents from one of the donor shards.
        {shardId2, {{shardId1, 5}}},
    };

    auto coordinatorDoc = makeCoordinatorDocument(docsToCopy, docsCopied);
    auto future =
        mockRecipientCloningMetricsResponses(coordinatorDoc.getReshardingUUID(), docsCopied);

    ReshardingCoordinatorExternalStateImpl externalState;
    externalState.verifyClonedCollection(
        operationContext(), getTaskExecutor(), getCancellationToken(), coordinatorDoc);
    future.get();
}

TEST_F(ReshardingCoordinatorServiceExternalStateTest,
       VerifyClonedCollectionFailure_MismatchingTotal) {
    DocumentsToCopyMap docsToCopy{
        {shardId0, 10},
    };
    DocumentsCopiedMap docsCopied{
        {shardId0, {{shardId0, 9}}},
    };

    auto coordinatorDoc = makeCoordinatorDocument(docsToCopy, docsCopied);
    auto future =
        mockRecipientCloningMetricsResponses(coordinatorDoc.getReshardingUUID(), docsCopied);

    ReshardingCoordinatorExternalStateImpl externalState;
    ASSERT_THROWS_CODE(
        externalState.verifyClonedCollection(
            operationContext(), getTaskExecutor(), getCancellationToken(), coordinatorDoc),
        DBException,
        9929901);
    future.get();
}

TEST_F(ReshardingCoordinatorServiceExternalStateTest,
       VerifyClonedCollectionFailure_MismatchingPerDonorWrongCount) {
    DocumentsToCopyMap docsToCopy{
        {shardId0, 10},
        {shardId1, 20},
    };
    DocumentsCopiedMap docsCopied{
        {shardId1, {{shardId0, 5}, {shardId1, 11}}},
        {shardId2, {{shardId0, 4}, {shardId1, 10}}},
    };

    auto coordinatorDoc = makeCoordinatorDocument(docsToCopy, docsCopied);
    auto future =
        mockRecipientCloningMetricsResponses(coordinatorDoc.getReshardingUUID(), docsCopied);

    ReshardingCoordinatorExternalStateImpl externalState;
    ASSERT_THROWS_CODE(
        externalState.verifyClonedCollection(
            operationContext(), getTaskExecutor(), getCancellationToken(), coordinatorDoc),
        DBException,
        9929901);
    future.get();
}

TEST_F(ReshardingCoordinatorServiceExternalStateTest,
       VerifyClonedCollectionFailure_MismatchingPerDonorWrongShard) {
    DocumentsToCopyMap docsToCopy{
        {shardId0, 10},
        {shardId1, 0},
    };
    DocumentsCopiedMap docsCopied{
        {shardId1, {{shardId0, 0}, {shardId1, 10}}},
    };

    auto coordinatorDoc = makeCoordinatorDocument(docsToCopy, docsCopied);
    auto future =
        mockRecipientCloningMetricsResponses(coordinatorDoc.getReshardingUUID(), docsCopied);

    ReshardingCoordinatorExternalStateImpl externalState;
    ASSERT_THROWS_CODE(
        externalState.verifyClonedCollection(
            operationContext(), getTaskExecutor(), getCancellationToken(), coordinatorDoc),
        DBException,
        9929901);
    future.get();
}

TEST_F(ReshardingCoordinatorServiceExternalStateTest,
       VerifyClonedCollectionFailure_MismatchingPerDonorAdditionalShard) {
    DocumentsToCopyMap docsToCopy{
        {shardId0, 10},
    };
    DocumentsCopiedMap docsCopied{
        {shardId1, {{shardId0, 10}, {shardId1, 5}}},
    };

    auto coordinatorDoc = makeCoordinatorDocument(docsToCopy, docsCopied);
    auto future =
        mockRecipientCloningMetricsResponses(coordinatorDoc.getReshardingUUID(), docsCopied);

    ReshardingCoordinatorExternalStateImpl externalState;
    ASSERT_THROWS_CODE(
        externalState.verifyClonedCollection(
            operationContext(), getTaskExecutor(), getCancellationToken(), coordinatorDoc),
        DBException,
        9929902);
    future.get();
}

TEST_F(ReshardingCoordinatorServiceExternalStateTest,
       VerifyClonedCollectionFailure_MissingDonorMetricsSingleDonorShard) {
    DocumentsToCopyMap docsToCopy{
        {shardId0, boost::none},
    };
    DocumentsCopiedMap docsCopied{
        {shardId0, {{shardId0, 10}}},
    };

    auto coordinatorDoc = makeCoordinatorDocument(docsToCopy, docsCopied);
    // Do not mock the recipient cloning metrics responses since the verification would fail before
    // the steps to fetch those metrics.

    ReshardingCoordinatorExternalStateImpl externalState;
    ASSERT_THROWS_CODE(
        externalState.verifyClonedCollection(
            operationContext(), getTaskExecutor(), getCancellationToken(), coordinatorDoc),
        DBException,
        9929907);
}

TEST_F(ReshardingCoordinatorServiceExternalStateTest,
       VerifyClonedCollectionFailure_MissingDonorMetricsMultipleDonorShards) {
    DocumentsToCopyMap docsToCopy{
        {shardId0, 30},
        {shardId1, boost::none},
    };
    DocumentsCopiedMap docsCopied{
        {shardId0, {{shardId0, 10}, {shardId1, 20}}},
    };

    auto coordinatorDoc = makeCoordinatorDocument(docsToCopy, docsCopied);
    // Do not mock the recipient cloning metrics responses since the verification would fail before
    // the steps to fetch those metrics.

    ReshardingCoordinatorExternalStateImpl externalState;
    ASSERT_THROWS_CODE(
        externalState.verifyClonedCollection(
            operationContext(), getTaskExecutor(), getCancellationToken(), coordinatorDoc),
        DBException,
        9929907);
}

TEST_F(ReshardingCoordinatorServiceExternalStateTest,
       VerifyClonedCollectionFailure_MissingRecipientMetricsSingleRecipientShard) {
    DocumentsToCopyMap docsToCopy{
        {shardId0, 10},
    };
    DocumentsCopiedMap docsCopied{
        {shardId0, {{shardId0, boost::none}}},
    };

    auto coordinatorDoc = makeCoordinatorDocument(docsToCopy, docsCopied);
    auto future =
        mockRecipientCloningMetricsResponses(coordinatorDoc.getReshardingUUID(), docsCopied);

    ReshardingCoordinatorExternalStateImpl externalState;
    ASSERT_THROWS_CODE(
        externalState.verifyClonedCollection(
            operationContext(), getTaskExecutor(), getCancellationToken(), coordinatorDoc),
        DBException,
        9929909);
    future.get();
}

TEST_F(ReshardingCoordinatorServiceExternalStateTest,
       VerifyClonedCollectionFailure_MissingRecipientMetricsMultipleRecipientShards) {
    DocumentsToCopyMap docsToCopy{
        {shardId0, 10},
    };
    DocumentsCopiedMap docsCopied{
        {shardId0, {{shardId0, 10}, {shardId1, boost::none}}},
    };

    auto coordinatorDoc = makeCoordinatorDocument(docsToCopy, docsCopied);
    auto future =
        mockRecipientCloningMetricsResponses(coordinatorDoc.getReshardingUUID(), docsCopied);

    ReshardingCoordinatorExternalStateImpl externalState;
    ASSERT_THROWS_CODE(
        externalState.verifyClonedCollection(
            operationContext(), getTaskExecutor(), getCancellationToken(), coordinatorDoc),
        DBException,
        9929909);
    future.get();
}

TEST_F(ReshardingCoordinatorServiceExternalStateTest, VerifyFinalCollectionSuccess_Basic) {
    DocumentsFinalMap docsFinalOnDonors{
        {shardId0, 10},
    };
    DocumentsFinalMap docsFinalOnRecipients{
        {shardId0, 10},
    };

    auto coordinatorDoc = makeCoordinatorDocument(docsFinalOnDonors, docsFinalOnRecipients);

    ReshardingCoordinatorExternalStateImpl externalState;
    externalState.verifyFinalCollection(operationContext(), coordinatorDoc);
}

TEST_F(ReshardingCoordinatorServiceExternalStateTest,
       VerifyFinalCollectionSuccess_NoDocsOnSubsetOfDonorShards) {
    DocumentsFinalMap docsFinalOnDonors{
        {shardId0, 10},
        {shardId1, 0},
    };
    DocumentsFinalMap docsFinalOnRecipients{
        {shardId0, 10},
    };

    auto coordinatorDoc = makeCoordinatorDocument(docsFinalOnDonors, docsFinalOnRecipients);

    ReshardingCoordinatorExternalStateImpl externalState;
    externalState.verifyFinalCollection(operationContext(), coordinatorDoc);
}

TEST_F(ReshardingCoordinatorServiceExternalStateTest,
       VerifyFinalCollectionSuccess_NoDocsOnSubsetOfRecipientShards) {
    DocumentsFinalMap docsFinalOnDonors{
        {shardId0, 10},
    };
    DocumentsFinalMap docsFinalOnRecipients{
        {shardId0, 10},
        {shardId1, 0},
    };

    auto coordinatorDoc = makeCoordinatorDocument(docsFinalOnDonors, docsFinalOnRecipients);

    ReshardingCoordinatorExternalStateImpl externalState;
    externalState.verifyFinalCollection(operationContext(), coordinatorDoc);
}

TEST_F(ReshardingCoordinatorServiceExternalStateTest, VerifyFinalCollectionSuccess_Mixed) {
    DocumentsFinalMap docsFinalOnDonors{
        {shardId0, 10},
        {shardId1, 20},
        {shardId2, 0},
    };
    DocumentsFinalMap docsFinalOnRecipients{
        {shardId0, 11},
        {shardId1, 0},
        {shardId2, 19},
    };

    auto coordinatorDoc = makeCoordinatorDocument(docsFinalOnDonors, docsFinalOnRecipients);

    ReshardingCoordinatorExternalStateImpl externalState;
    externalState.verifyFinalCollection(operationContext(), coordinatorDoc);
}

TEST_F(ReshardingCoordinatorServiceExternalStateTest,
       VerifyFinalCollectionFailure_SingleDonorAndRecipientShard) {
    DocumentsFinalMap docsFinalOnDonors{
        {shardId0, 10},
    };
    DocumentsFinalMap docsFinalOnRecipients{
        {shardId0, 9},
    };

    auto coordinatorDoc = makeCoordinatorDocument(docsFinalOnDonors, docsFinalOnRecipients);

    ReshardingCoordinatorExternalStateImpl externalState;
    ASSERT_THROWS_CODE(externalState.verifyFinalCollection(operationContext(), coordinatorDoc),
                       DBException,
                       9929906);
}

TEST_F(ReshardingCoordinatorServiceExternalStateTest,
       VerifyFinalCollectionFailure_MultipleDonorShards) {
    DocumentsFinalMap docsFinalOnDonors{
        {shardId0, 10},
        {shardId1, 20},
    };
    DocumentsFinalMap docsFinalOnRecipients{
        {shardId0, 29},
    };

    auto coordinatorDoc = makeCoordinatorDocument(docsFinalOnDonors, docsFinalOnRecipients);

    ReshardingCoordinatorExternalStateImpl externalState;
    ASSERT_THROWS_CODE(externalState.verifyFinalCollection(operationContext(), coordinatorDoc),
                       DBException,
                       9929906);
}

TEST_F(ReshardingCoordinatorServiceExternalStateTest,
       VerifyFinalCollectionFailure_MultipleRecipientShards) {
    DocumentsFinalMap docsFinalOnDonors{
        {shardId0, 10},
    };
    DocumentsFinalMap docsFinalOnRecipients{
        {shardId0, 10},
        {shardId1, 1},
    };

    auto coordinatorDoc = makeCoordinatorDocument(docsFinalOnDonors, docsFinalOnRecipients);

    ReshardingCoordinatorExternalStateImpl externalState;
    ASSERT_THROWS_CODE(externalState.verifyFinalCollection(operationContext(), coordinatorDoc),
                       DBException,
                       9929906);
}

TEST_F(ReshardingCoordinatorServiceExternalStateTest,
       VerifyFinalCollectionFailure_MissingDonorMetricsSingleDonorShard) {
    DocumentsFinalMap docsFinalOnDonors{
        {shardId0, boost::none},
    };
    DocumentsFinalMap docsFinalOnRecipients{
        {shardId0, 10},
    };

    auto coordinatorDoc = makeCoordinatorDocument(docsFinalOnDonors, docsFinalOnRecipients);

    ReshardingCoordinatorExternalStateImpl externalState;
    ASSERT_THROWS_CODE(externalState.verifyFinalCollection(operationContext(), coordinatorDoc),
                       DBException,
                       9929904);
}

TEST_F(ReshardingCoordinatorServiceExternalStateTest,
       VerifyFinalCollectionFailure_MissingDonorMetricsMultipleDonorShards) {
    DocumentsFinalMap docsFinalOnDonors{
        {shardId0, 30},
        {shardId1, boost::none},
    };
    DocumentsFinalMap docsFinalOnRecipients{
        {shardId0, 10},
        {shardId1, 20},
    };

    auto coordinatorDoc = makeCoordinatorDocument(docsFinalOnDonors, docsFinalOnRecipients);

    ReshardingCoordinatorExternalStateImpl externalState;
    ASSERT_THROWS_CODE(externalState.verifyFinalCollection(operationContext(), coordinatorDoc),
                       DBException,
                       9929904);
}

TEST_F(ReshardingCoordinatorServiceExternalStateTest,
       VerifyFinalCollectionFailure_MissingRecipientMetricsSingleRecipientShard) {
    DocumentsFinalMap docsFinalOnDonors{
        {shardId0, 10},
    };
    DocumentsFinalMap docsFinalOnRecipients{
        {shardId0, boost::none},
    };

    auto coordinatorDoc = makeCoordinatorDocument(docsFinalOnDonors, docsFinalOnRecipients);

    ReshardingCoordinatorExternalStateImpl externalState;
    ASSERT_THROWS_CODE(externalState.verifyFinalCollection(operationContext(), coordinatorDoc),
                       DBException,
                       9929905);
}

TEST_F(ReshardingCoordinatorServiceExternalStateTest,
       VerifyFinalCollectionFailure_MissingRecipientMetricsMultipleRecipientShards) {
    DocumentsFinalMap docsFinalOnDonors{
        {shardId0, 10},
    };
    DocumentsFinalMap docsFinalOnRecipients{
        {shardId0, 10},
        {shardId1, boost::none},
    };

    auto coordinatorDoc = makeCoordinatorDocument(docsFinalOnDonors, docsFinalOnRecipients);

    ReshardingCoordinatorExternalStateImpl externalState;
    ASSERT_THROWS_CODE(externalState.verifyFinalCollection(operationContext(), coordinatorDoc),
                       DBException,
                       9929905);
}

TEST_F(ReshardingCoordinatorServiceExternalStateTest, GetDocumentsToCopyFromDonors_SuccessBasic) {
    std::map<ShardId, ShardVersion> shardVersions{
        {shardId0, shardVersion0},
        {shardId1, shardVersion1},
    };
    auto count0 = 123;
    auto count1 = 456;
    std::map<ShardId, std::vector<BSONObj>> donorResponseDocs{
        {shardId0, {BSON("count" << count0)}}, {shardId1, {BSON("count" << count1)}}};

    auto future = mockDonorCloningMetricsResponses(donorResponseDocs);

    ReshardingCoordinatorExternalStateImpl externalState;
    auto docsToCopy =
        externalState.getDocumentsToCopyFromDonors(operationContext(),
                                                   **taskExecutor,
                                                   operationContext()->getCancellationToken(),
                                                   reshardingUUID,
                                                   sourceNss,
                                                   cloneTimestamp,
                                                   shardVersions);
    ASSERT_EQ(docsToCopy[shardId0], count0);
    ASSERT_EQ(docsToCopy[shardId1], count1);

    future.get();
}

TEST_F(ReshardingCoordinatorServiceExternalStateTest,
       GetDocumentsToCopyFromDonors_SuccessNoDocuments) {
    std::map<ShardId, ShardVersion> shardVersions{
        {shardId0, shardVersion0},
    };
    std::map<ShardId, std::vector<BSONObj>> donorResponseDocs{{shardId0, {}}};

    auto future = mockDonorCloningMetricsResponses(donorResponseDocs);

    ReshardingCoordinatorExternalStateImpl externalState;
    auto docsToCopy =
        externalState.getDocumentsToCopyFromDonors(operationContext(),
                                                   **taskExecutor,
                                                   operationContext()->getCancellationToken(),
                                                   reshardingUUID,
                                                   sourceNss,
                                                   cloneTimestamp,
                                                   shardVersions);

    ASSERT_EQ(docsToCopy.size(), 1);
    ASSERT_EQ(docsToCopy[shardId0], 0);
}

TEST_F(ReshardingCoordinatorServiceExternalStateTest, GetDocumentsDeltaFromDonors_SuccessBasic) {
    std::vector<ShardId> shardIds{shardId0, shardId1};
    std::vector<ShardVersion> shardVersions{shardVersion0, shardVersion1};
    auto delta0 = 123;
    auto delta1 = 0;

    auto future = mockDonorDeltaMetricsResponses({{shardId0, delta0}, {shardId1, delta1}});

    ReshardingCoordinatorExternalStateImpl externalState;
    auto docsDelta =
        externalState.getDocumentsDeltaFromDonors(operationContext(),
                                                  **taskExecutor,
                                                  operationContext()->getCancellationToken(),
                                                  reshardingUUID,
                                                  sourceNss,
                                                  shardIds);
    ASSERT_EQ(docsDelta[shardId0], delta0);
    ASSERT_EQ(docsDelta[shardId1], delta1);

    future.get();
}

}  // namespace

}  // namespace resharding
}  // namespace mongo
