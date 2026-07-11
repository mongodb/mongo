// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/resharding/resharding_coordinator_service_external_state.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/s/resharding/recipient_resume_document_gen.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/s/resharding/shardsvr_resharding_commands_gen.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/executor/mock_async_rpc.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
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
        shard0.setHandle(ShardHandle{ShardId(shardId0.toString()), boost::none});
        shard0.setHost(shardId0.toString() + ":1234");
        ShardType shard1;
        shard1.setHandle(ShardHandle{ShardId(shardId1.toString()), boost::none});
        shard1.setHost(shardId1.toString() + ":1234");
        ShardType shard2;
        shard2.setHandle(ShardHandle{ShardId(shardId2.toString()), boost::none});
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
        executor = executor::ThreadPoolTaskExecutor::create(
            ThreadPool::make({
                .poolName = "ReshardingCoordinatorExternalStateTest",
            }),
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
            RecipientShardEntry recipientEntry(recipientShardId, {});
            recipientEntry.setDocumentsFinal(recipientDocsFinal);
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
                    BSON("$match" << BSON(
                             fmt::format("{}.{}",
                                         ReshardingRecipientResumeData::kIdFieldName,
                                         ReshardingRecipientResumeDataId::kReshardingUUIDFieldName)
                             << reshardingUUID)));
                ASSERT_BSONOBJ_EQ(
                    pipeline[1],
                    BSON("$group" << BSON(
                             "_id"
                             << BSONNULL << "pairs"
                             << BSON("$push" << BSON(
                                         "k"
                                         << fmt::format(
                                                "${}.{}",
                                                ReshardingRecipientResumeData::kIdFieldName,
                                                ReshardingRecipientResumeDataId::kShardIdFieldName)

                                         << "v"
                                         << fmt::format("${}",
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

    auto mockDonorCloneCountResponses(const std::map<ShardId, int64_t>& counts) {
        std::vector<Future<void>> expectations;

        for (auto donorIter = counts.begin(); donorIter != counts.end(); ++donorIter) {
            auto donorShardId = donorIter->first;
            auto donorCount = donorIter->second;

            auto asyncRPCRunner = dynamic_cast<async_rpc::AsyncMockAsyncRPCRunner*>(
                async_rpc::detail::AsyncRPCRunner::get(operationContext()->getServiceContext()));

            auto matcher = [this, donorShardId = donorShardId](
                               const async_rpc::AsyncMockAsyncRPCRunner::Request& req) {
                ShardId shardId{req.target.host()};

                if (shardId != donorShardId) {
                    return false;
                }

                auto cmd = ShardsvrReshardingDonorGetCloneCount::parse(
                    req.cmdBSON.addFields(BSON("$db" << req.dbName)),
                    IDLParserContext("mockDonorCloneCountResponses"));

                ASSERT_EQ(cmd.getCommandParameter(), sourceNss);
                ASSERT_EQ(cmd.getReshardingUUID(), reshardingUUID);
                ASSERT_EQ(cmd.getCloneTimestamp(), cloneTimestamp);
                ASSERT_BSONOBJ_EQ(
                    cmd.getReadConcern()->toBSON(),
                    BSON("readConcern" << BSON("level" << "snapshot"
                                                       << "atClusterTime" << cloneTimestamp)));
                ASSERT(cmd.getReadPreference()->equals(
                    ReadPreferenceSetting{ReadPreference::SecondaryPreferred}));
                ASSERT_EQ(cmd.getShardVersion(), shardVersions.find(donorShardId)->second);

                return true;
            };

            ShardsvrReshardingDonorGetCloneCountResponse response(donorCount);
            expectations.push_back(asyncRPCRunner
                                       ->expect(matcher,
                                                response.toBSON().addFields(BSON("ok" << 1)),
                                                "mockDonorCloneCountResponses")
                                       .unsafeToInlineFuture());
        }

        return whenAllSucceed(std::move(expectations));
    }

    auto mockDonorCloneCountErrorResponse(const ShardId& donorShardId, ErrorCodes::Error code) {
        auto asyncRPCRunner = dynamic_cast<async_rpc::AsyncMockAsyncRPCRunner*>(
            async_rpc::detail::AsyncRPCRunner::get(operationContext()->getServiceContext()));

        auto matcher = [donorShardId](const async_rpc::AsyncMockAsyncRPCRunner::Request& req) {
            return ShardId{req.target.host()} == donorShardId;
        };

        auto response = BSON("ok" << 0 << "code" << code << "errmsg"
                                  << "mock donor clone count error");
        return asyncRPCRunner
            ->expect(matcher, std::move(response), "mockDonorCloneCountErrorResponse")
            .unsafeToInlineFuture();
    }

    template <typename CmdType, typename ResponseType>
    auto mockDeltaMetricsResponses(const std::map<ShardId, int64_t>& docsDelta,
                                   const NamespaceString& expectedNss,
                                   std::string label) {
        std::vector<Future<void>> expectations;

        for (auto iter = docsDelta.begin(); iter != docsDelta.end(); ++iter) {
            auto participantShardId = iter->first;
            auto participantDocsDelta = iter->second;

            auto asyncRPCRunner = dynamic_cast<async_rpc::AsyncMockAsyncRPCRunner*>(
                async_rpc::detail::AsyncRPCRunner::get(operationContext()->getServiceContext()));

            auto matcher = [this, participantShardId, expectedNss, label](
                               const async_rpc::AsyncMockAsyncRPCRunner::Request& req) {
                ShardId shardId{req.target.host()};

                if (shardId != participantShardId) {
                    return false;
                }

                auto fetchCmd = CmdType::parse(req.cmdBSON.addFields(BSON("$db" << req.dbName)),
                                               IDLParserContext(label));

                ASSERT_EQ(fetchCmd.getCommandParameter(), expectedNss);
                ASSERT_EQ(fetchCmd.getReshardingUUID(), reshardingUUID);
                return true;
            };

            ResponseType response(participantDocsDelta);
            expectations.push_back(
                asyncRPCRunner->expect(matcher, response.toBSON().addFields(BSON("ok" << 1)), label)
                    .unsafeToInlineFuture());
        }

        return whenAllSucceed(std::move(expectations));
    }

    auto mockDonorDeltaMetricsResponses(const std::map<ShardId, int64_t>& docsDelta) {
        return mockDeltaMetricsResponses<ShardsvrReshardingDonorFetchFinalCollectionStats,
                                         ShardsvrReshardingDonorFetchFinalCollectionStatsResponse>(
            docsDelta, sourceNss, "mockDonorDeltaMetricsResponses");
    }

    auto mockRecipientDeltaMetricsResponses(const std::map<ShardId, int64_t>& docsDelta) {
        return mockDeltaMetricsResponses<
            ShardsvrReshardingRecipientFetchFinalCollectionStats,
            ShardsvrReshardingRecipientFetchFinalCollectionStatsResponse>(
            docsDelta, tempNss, "mockRecipientDeltaMetricsResponses");
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
                       ErrorCodes::ReshardingValidationIncompleteData);
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
                       ErrorCodes::ReshardingValidationIncompleteData);
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
                       ErrorCodes::ReshardingValidationIncompleteData);
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
                       ErrorCodes::ReshardingValidationIncompleteData);
}

TEST_F(ReshardingCoordinatorServiceExternalStateTest, GetDocumentsToCopyFromDonors_SuccessBasic) {
    std::map<ShardId, ShardVersion> shardVersions{
        {shardId0, shardVersion0},
        {shardId1, shardVersion1},
    };
    auto count0 = 123;
    auto count1 = 456;

    auto future = mockDonorCloneCountResponses({{shardId0, count0}, {shardId1, count1}});

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

    auto future = mockDonorCloneCountResponses({{shardId0, 0}});

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

TEST_F(ReshardingCoordinatorServiceExternalStateTest,
       GetDocumentsToCopyFromDonors_PropagatesDonorError) {
    std::map<ShardId, ShardVersion> shardVersions{
        {shardId0, shardVersion0},
    };

    auto future = mockDonorCloneCountErrorResponse(shardId0, ErrorCodes::IllegalOperation);

    ReshardingCoordinatorExternalStateImpl externalState;
    ASSERT_THROWS_CODE(
        externalState.getDocumentsToCopyFromDonors(operationContext(),
                                                   **taskExecutor,
                                                   operationContext()->getCancellationToken(),
                                                   reshardingUUID,
                                                   sourceNss,
                                                   cloneTimestamp,
                                                   shardVersions),
        DBException,
        ErrorCodes::IllegalOperation);

    future.get();
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

TEST_F(ReshardingCoordinatorServiceExternalStateTest,
       GetDocumentsDeltaFromRecipients_SuccessBasic) {
    std::vector<ShardId> shardIds{shardId0, shardId1};
    auto delta0 = 123;
    auto delta1 = 0;

    auto future = mockRecipientDeltaMetricsResponses({{shardId0, delta0}, {shardId1, delta1}});

    ReshardingCoordinatorExternalStateImpl externalState;
    auto docsDelta =
        externalState.getDocumentsDeltaFromRecipients(operationContext(),
                                                      **taskExecutor,
                                                      operationContext()->getCancellationToken(),
                                                      reshardingUUID,
                                                      tempNss,
                                                      shardIds);
    ASSERT_EQ(docsDelta[shardId0], delta0);
    ASSERT_EQ(docsDelta[shardId1], delta1);

    future.get();
}

}  // namespace

}  // namespace resharding
}  // namespace mongo
