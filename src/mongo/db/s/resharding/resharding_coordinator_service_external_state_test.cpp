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
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/db/s/resharding/recipient_resume_document_gen.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/unittest/assert.h"


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
        for (auto [donorShardId, donorDocsToCopy] : docsToCopy) {
            DonorShardEntry donorEntry(donorShardId, {});
            donorEntry.setDocumentsToCopy(donorDocsToCopy);
            donorShards.emplace_back(donorEntry);
        }

        std::vector<RecipientShardEntry> recipientShards;
        for (auto [recipientShardId, _] : docsCopied) {
            RecipientShardEntry recipientEntry(recipientShardId, {});
            recipientShards.emplace_back(recipientEntry);
        }

        ReshardingCoordinatorDocument coordinatorDoc;
        coordinatorDoc.setCommonReshardingMetadata(
            {_reshardingUUID, _sourceNss, _sourceUUID, _tempNss, _shardKey});
        coordinatorDoc.setState(CoordinatorStateEnum::kCloning);
        coordinatorDoc.setDonorShards(donorShards);
        coordinatorDoc.setRecipientShards(recipientShards);

        return coordinatorDoc;
    }

    ReshardingCoordinatorDocument makeCoordinatorDocument(
        const DocumentsFinalMap& docsFinalOnDonors,
        const DocumentsFinalMap& docsFinalOnRecipients) {
        std::vector<DonorShardEntry> donorShards;
        for (auto [donorShardId, donorDocsFinal] : docsFinalOnDonors) {
            DonorShardEntry donorEntry(donorShardId, {});
            donorEntry.setDocumentsFinal(donorDocsFinal);
            donorShards.emplace_back(donorEntry);
        }

        std::vector<RecipientShardEntry> recipientShards;
        for (auto [recipientShardId, recipientDocsFinal] : docsFinalOnRecipients) {
            RecipientShardContext mutableState;
            mutableState.setTotalNumDocuments(recipientDocsFinal);
            RecipientShardEntry recipientEntry(recipientShardId, mutableState);
            recipientShards.emplace_back(recipientEntry);
        }

        ReshardingCoordinatorDocument coordinatorDoc;
        coordinatorDoc.setCommonReshardingMetadata(
            {_reshardingUUID, _sourceNss, _sourceUUID, _tempNss, _shardKey});
        coordinatorDoc.setState(CoordinatorStateEnum::kApplying);
        coordinatorDoc.setDonorShards(donorShards);
        coordinatorDoc.setRecipientShards(recipientShards);

        return coordinatorDoc;
    }

    std::vector<BSONObj> makeRecipientResumeDataDocuments(
        const UUID& reshardingUUID, const RecipientDocumentsCopiedMap& docsCopied) {
        std::vector<BSONObj> docs;

        for (auto donorIter = docsCopied.begin(); donorIter != docsCopied.end(); ++donorIter) {
            auto donorShardId = donorIter->first;
            auto donorDocsCopied = donorIter->second;

            ReshardingRecipientResumeDataId resumeDataId({reshardingUUID, donorShardId});
            ReshardingRecipientResumeData resumeDataDoc(resumeDataId);
            if (donorDocsCopied) {
                resumeDataDoc.setDocumentsCopied(*donorDocsCopied);
            }
            docs.push_back(resumeDataDoc.toBSON());
        }

        return docs;
    }

    auto mockRecipientCloningMetricsResponses(const UUID& reshardingUUID,
                                              const DocumentsCopiedMap& docsCopied) {
        return launchAsync([&]() {
            for (auto recipientIter = docsCopied.begin(); recipientIter != docsCopied.end();
                 ++recipientIter) {
                auto recipientShardId = recipientIter->first;
                auto recipientDocsCopied = recipientIter->second;

                onCommand([&](const auto& request) {
                    std::vector<BSONObj> docs =
                        makeRecipientResumeDataDocuments(reshardingUUID, recipientDocsCopied);
                    CursorResponse response(NamespaceString::kConfigReshardingOperationsNamespace,
                                            0 /* cursorId */,
                                            docs);
                    return response.toBSON(CursorResponse::ResponseType::InitialResponse);
                });
            }
        });
    }

protected:
    const ShardId shardId0{"shard0"};
    const ShardId shardId1{"shard1"};
    const ShardId shardId2{"shard2"};

    const UUID _reshardingUUID = UUID::gen();
    const NamespaceString _sourceNss =
        NamespaceString::createNamespaceString_forTest("testDb", "testColl");
    const UUID _sourceUUID = UUID::gen();
    const NamespaceString _tempNss =
        resharding::constructTemporaryReshardingNss(_sourceNss, _sourceUUID);
    const BSONObj _shardKey = BSON("skey" << 1);
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
    externalState.verifyClonedCollection(operationContext(), coordinatorDoc);
    future.default_timed_get();
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
    externalState.verifyClonedCollection(operationContext(), coordinatorDoc);
    future.default_timed_get();
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
    externalState.verifyClonedCollection(operationContext(), coordinatorDoc);
    future.default_timed_get();
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
    externalState.verifyClonedCollection(operationContext(), coordinatorDoc);
    future.default_timed_get();
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
    ASSERT_THROWS_CODE(externalState.verifyClonedCollection(operationContext(), coordinatorDoc),
                       DBException,
                       9929901);
    future.default_timed_get();
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
    ASSERT_THROWS_CODE(externalState.verifyClonedCollection(operationContext(), coordinatorDoc),
                       DBException,
                       9929901);
    future.default_timed_get();
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
    ASSERT_THROWS_CODE(externalState.verifyClonedCollection(operationContext(), coordinatorDoc),
                       DBException,
                       9929901);
    future.default_timed_get();
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
    ASSERT_THROWS_CODE(externalState.verifyClonedCollection(operationContext(), coordinatorDoc),
                       DBException,
                       9929902);
    future.default_timed_get();
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
    ASSERT_THROWS_CODE(externalState.verifyClonedCollection(operationContext(), coordinatorDoc),
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
    ASSERT_THROWS_CODE(externalState.verifyClonedCollection(operationContext(), coordinatorDoc),
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
    ASSERT_THROWS_CODE(externalState.verifyClonedCollection(operationContext(), coordinatorDoc),
                       DBException,
                       9929909);
    future.default_timed_get();
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
    ASSERT_THROWS_CODE(externalState.verifyClonedCollection(operationContext(), coordinatorDoc),
                       DBException,
                       9929909);
    future.default_timed_get();
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

}  // namespace

}  // namespace resharding
}  // namespace mongo
