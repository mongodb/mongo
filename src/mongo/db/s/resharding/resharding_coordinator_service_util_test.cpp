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
#include "mongo/db/s/resharding/resharding_coordinator_service_util.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"
namespace mongo {
namespace resharding {

class ReshardingCoordinatorServiceUtilTest : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
    }

    void tearDown() override {
        ServiceContextTest::tearDown();
    }
};

TEST_F(ReshardingCoordinatorServiceUtilTest,
       GenerateBatchedCommandRequestForConfigCollectionsForTempNss) {
    auto opCtx = makeOperationContext();
    ReshardingCoordinatorDocument coordinatorDoc;
    // Set the common resharding metadata.
    NamespaceString sourceNss = NamespaceString::createNamespaceString_forTest("db.foo");
    NamespaceString tempNss =
        NamespaceString::createNamespaceString_forTest("db.system.resharding");
    CommonReshardingMetadata commonReshardingMetadata(
        UUID::gen(), sourceNss, UUID::gen(), tempNss, KeyPattern(BSON("tempShardKey" << 1)));
    commonReshardingMetadata.setStartTime(Date_t::now());
    commonReshardingMetadata.setPerformVerification(true);

    coordinatorDoc.setCommonReshardingMetadata(std::move(commonReshardingMetadata));

    // Set additional fields.
    coordinatorDoc.setApproxBytesToCopy(1024);                // Approximate bytes to copy
    coordinatorDoc.setApproxDocumentsToCopy(100);             // Approximate documents to copy
    coordinatorDoc.setCloneTimestamp(Timestamp(1234, 5678));  // Clone timestamp

    // Add donor shards.
    std::vector<DonorShardEntry> donorShards;
    DonorShardContext donorContext;
    donorContext.setState(DonorStateEnum::kPreparingToDonate);

    donorShards.emplace_back(ShardId("donorShard1"), donorContext);
    donorShards.emplace_back(ShardId("donorShard2"), donorContext);
    coordinatorDoc.setDonorShards(std::move(donorShards));

    // Add recipient shards.
    std::vector<RecipientShardEntry> recipientShards;
    RecipientShardContext recipientContext;
    recipientContext.setState(RecipientStateEnum::kCloning);
    recipientShards.emplace_back(ShardId("recipientShard1"), recipientContext);
    recipientShards.emplace_back(ShardId("recipientShard2"), recipientContext);
    coordinatorDoc.setRecipientShards(std::move(recipientShards));

    // Set collation.
    BSONObj collation = BSON("locale" << "simple");
    coordinatorDoc.setCollation(collation);

    // Initialize additional fields as needed.
    boost::optional<ChunkVersion> chunkVersion =
        ChunkVersion(CollectionGeneration{OID::gen(), Timestamp(5, 0)}, CollectionPlacement(10, 1));
    boost::optional<bool> isUnsplittable = true;

    for (auto& state : {
             CoordinatorStateEnum::kUnused,
             CoordinatorStateEnum::kInitializing,
             CoordinatorStateEnum::kPreparingToDonate,
             CoordinatorStateEnum::kCloning,
             CoordinatorStateEnum::kApplying,
             CoordinatorStateEnum::kBlockingWrites,
             CoordinatorStateEnum::kAborting,
             CoordinatorStateEnum::kCommitting,
             CoordinatorStateEnum::kQuiesced,
             CoordinatorStateEnum::kDone,
         }) {
        coordinatorDoc.setState(state);
        auto request = generateBatchedCommandRequestForConfigCollectionsForTempNss(
            opCtx.get(), coordinatorDoc, chunkVersion, collation, isUnsplittable);

        if (state == CoordinatorStateEnum::kPreparingToDonate) {
            ASSERT_TRUE(request.getBatchType() == BatchedCommandRequest::BatchType_Insert);

            auto insertDocs = request.getInsertRequest().getDocuments();
            ASSERT_EQ(insertDocs.size(), 1);

            auto expectedDoc =
                resharding::createTempReshardingCollectionType(
                    opCtx.get(), coordinatorDoc, chunkVersion.value(), collation, isUnsplittable)
                    .toBSON();

            auto actualDoc = insertDocs[0];

            // Compare specific fields individually.
            ASSERT_BSONELT_EQ(actualDoc["_id"], expectedDoc["_id"]);
            ASSERT_BSONELT_EQ(actualDoc["uuid"], expectedDoc["uuid"]);
            ASSERT_BSONELT_EQ(actualDoc["key"], expectedDoc["key"]);
            ASSERT_BSONELT_EQ(actualDoc["defaultCollation"], expectedDoc["defaultCollation"]);
            ASSERT_BSONELT_EQ(actualDoc["reshardingFields"], expectedDoc["reshardingFields"]);
        } else if (state == CoordinatorStateEnum::kCloning) {
            ASSERT_TRUE(request.getBatchType() == BatchedCommandRequest::BatchType_Update);

            BSONObj expectedQuery = BSON(
                CollectionType::kNssFieldName << NamespaceStringUtil::serialize(
                    coordinatorDoc.getTempReshardingNss(), SerializationContext::stateDefault()));
            BSONArrayBuilder donorShardsBuilder;
            for (const auto& donorShard : coordinatorDoc.getDonorShards()) {
                donorShardsBuilder.append(BSON("shardId" << donorShard.getId()));
            }
            BSONArray donorShardsArray = donorShardsBuilder.arr();
            BSONObj expectedUpdate =
                BSON("$set" << BSON("reshardingFields.state"
                                    << CoordinatorState_serializer(state)
                                    << "reshardingFields.recipientFields.approxDocumentsToCopy"
                                    << coordinatorDoc.getApproxDocumentsToCopy().value()
                                    << "reshardingFields.recipientFields.approxBytesToCopy"
                                    << coordinatorDoc.getApproxBytesToCopy().value()
                                    << "reshardingFields.recipientFields.cloneTimestamp"
                                    << coordinatorDoc.getCloneTimestamp().value()
                                    << "reshardingFields.recipientFields.donorShards"
                                    << donorShardsArray));

            ASSERT_EQ(request.getUpdateRequest().getUpdates().size(), 1);
            auto updateEntry = request.getUpdateRequest().getUpdates()[0].toBSON();

            // The lastmod field has a dynamic value, so remove it for comparison.
            auto setBSON =
                updateEntry.getObjectField("u").getObjectField("$set").removeField("lastmod");

            ASSERT_BSONOBJ_EQ(updateEntry.getObjectField("q"), expectedQuery);
            ASSERT_BSONOBJ_EQ(setBSON, expectedUpdate.getObjectField("$set"));
        } else if (state == CoordinatorStateEnum::kCommitting) {
            ASSERT_TRUE(request.getBatchType() == BatchedCommandRequest::BatchType_Delete);
            BSONObj expectedQuery = BSON(
                CollectionType::kNssFieldName << NamespaceStringUtil::serialize(
                    coordinatorDoc.getTempReshardingNss(), SerializationContext::stateDefault()));
            BSONObj expectedDeletes = BSON("q" << expectedQuery << "limit" << 1);

            BSONObj expectedBSON = BSON(
                "delete" << CollectionType::ConfigNS.coll() << "bypassDocumentValidation" << false
                         << "ordered" << true << "deletes" << BSON_ARRAY(expectedDeletes));

            auto requestBSON = request.toBSON();
            ASSERT_BSONOBJ_EQ(requestBSON, expectedBSON);
        } else {
            ASSERT_TRUE(request.getBatchType() == BatchedCommandRequest::BatchType_Update);
            ASSERT_EQ(request.getUpdateRequest().getUpdates().size(), 1);
            auto updateEntry = request.getUpdateRequest().getUpdates()[0].toBSON();

            BSONObj expectedQuery = BSON(
                CollectionType::kNssFieldName << NamespaceStringUtil::serialize(
                    coordinatorDoc.getTempReshardingNss(), SerializationContext::stateDefault()));
            ASSERT_BSONOBJ_EQ(updateEntry.getObjectField("q"), expectedQuery);

            BSONObjBuilder expectedBSONBuilder;
            BSONObjBuilder setBuilder(expectedBSONBuilder.subobjStart("$set"));
            setBuilder.append("reshardingFields.state", CoordinatorState_serializer(state));

            if (state == CoordinatorStateEnum::kAborting && coordinatorDoc.getAbortReason()) {
                setBuilder.append("reshardingFields.abortReason", *coordinatorDoc.getAbortReason());
                auto abortStatus = resharding::getStatusFromAbortReason(coordinatorDoc);
                setBuilder.append("reshardingFields.userCanceled",
                                  abortStatus == ErrorCodes::ReshardCollectionAborted);
            }
            setBuilder.done();
            BSONObj expectedBSON = expectedBSONBuilder.obj();
            auto setBSON =
                updateEntry.getObjectField("u").getObjectField("$set").removeField("lastmod");
            ASSERT_BSONOBJ_EQ(setBSON, expectedBSON.getObjectField("$set"));
        }
    }
}

TEST_F(ReshardingCoordinatorServiceUtilTest,
       AssertResultIsValidForWriteToConfigCollectionsForTempNssTestDeleteRequest) {
    auto request = BatchedCommandRequest::buildDeleteOp(
        CollectionType::ConfigNS,
        BSON(CollectionType::kNssFieldName << NamespaceStringUtil::serialize(
                 NamespaceString::createNamespaceString_forTest("db.foo"),
                 SerializationContext::stateDefault())),
        false  // multi
    );
    BSONObjBuilder resultOneDelete;
    resultOneDelete.append("n", 1);
    ASSERT_DOES_NOT_THROW(
        resharding::assertResultIsValidForUpdatesAndDeletes(request, resultOneDelete.obj()));

    BSONObjBuilder resultZeroDelete;
    resultZeroDelete.append("n", 0);
    ASSERT_DOES_NOT_THROW(
        resharding::assertResultIsValidForUpdatesAndDeletes(request, resultZeroDelete.obj()));

    BSONObjBuilder resultMultipleDeletes;
    resultMultipleDeletes.append("n", 2);
    ASSERT_THROWS(
        resharding::assertResultIsValidForUpdatesAndDeletes(request, resultMultipleDeletes.obj()),
        DBException);
}

TEST_F(ReshardingCoordinatorServiceUtilTest,
       AssertResultIsValidForWriteToConfigCollectionsForTempNssTestUpdateRequest) {
    auto request = BatchedCommandRequest::buildUpdateOp(
        CollectionType::ConfigNS,
        BSON(CollectionType::kNssFieldName << NamespaceStringUtil::serialize(
                 NamespaceString::createNamespaceString_forTest("db.foo"),
                 SerializationContext::stateDefault())),
        BSON("key" << 1 << "value" << 2),
        false,  // upsert
        false   // multi
    );
    BSONObjBuilder resultOneUpdate;
    resultOneUpdate.append("n", 1);
    ASSERT_DOES_NOT_THROW(
        resharding::assertResultIsValidForUpdatesAndDeletes(request, resultOneUpdate.obj()));

    BSONObjBuilder resultZeroUpdate;
    resultZeroUpdate.append("n", 0);
    ASSERT_THROWS(
        resharding::assertResultIsValidForUpdatesAndDeletes(request, resultZeroUpdate.obj()),
        DBException);

    BSONObjBuilder resultMultipleUpdates;
    resultMultipleUpdates.append("n", 2);
    ASSERT_THROWS(
        resharding::assertResultIsValidForUpdatesAndDeletes(request, resultMultipleUpdates.obj()),
        DBException);
}

TEST_F(ReshardingCoordinatorServiceUtilTest,
       AssertResultIsValidForWriteToConfigCollectionsForTempNssTestInsertRequest) {
    auto request = BatchedCommandRequest::buildInsertOp(
        CollectionType::ConfigNS, std::vector<BSONObj>{BSON("key" << 1 << "value" << 2)});
    BSONObj result;

    // We do not need to add anything to the result because we do not validate the result on insert.
    ASSERT_DOES_NOT_THROW(resharding::assertResultIsValidForUpdatesAndDeletes(request, result));
}

}  // namespace resharding
}  // namespace mongo
