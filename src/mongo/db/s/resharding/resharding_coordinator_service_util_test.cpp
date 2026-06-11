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

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/s/forwardable_operation_metadata.h"
#include "mongo/db/s/resharding/local_resharding_operations_registry.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/version_context.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/otel/traces/span/span.h"
#include "mongo/otel/traces/telemetry_context_serialization.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/version/releases.h"

#include <array>

#include <gtest/gtest.h>

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

    const NamespaceString kSourceNss = NamespaceString::createNamespaceString_forTest("db.foo");
    const NamespaceString kTempNss =
        NamespaceString::createNamespaceString_forTest("db.system.resharding");
    const BSONObj kShardKey = BSON("x" << 1);

    CommonReshardingMetadata makeMetadata(const UUID& reshardingUUID = UUID::gen()) {
        return CommonReshardingMetadata(
            reshardingUUID, kSourceNss, UUID::gen(), kTempNss, kShardKey);
    }

    ReshardingCoordinatorDocument makeTempNssTestCoordinatorDoc() {
        ReshardingCoordinatorDocument coordinatorDoc;
        auto commonReshardingMetadata = makeMetadata();
        commonReshardingMetadata.setStartTime(Date_t::now());
        commonReshardingMetadata.setPerformVerification(true);
        coordinatorDoc.setCommonReshardingMetadata(std::move(commonReshardingMetadata));

        coordinatorDoc.setApproxBytesToCopy(1024);
        coordinatorDoc.setApproxDocumentsToCopy(100);
        coordinatorDoc.setCloneTimestamp(Timestamp(1234, 5678));

        std::vector<DonorShardEntry> donorShards;
        DonorShardContext donorContext;
        donorContext.setState(DonorStateEnum::kPreparingToDonate);
        donorShards.emplace_back(ShardId("donorShard1"), donorContext);
        donorShards.emplace_back(ShardId("donorShard2"), donorContext);
        coordinatorDoc.setDonorShards(std::move(donorShards));

        std::vector<RecipientShardEntry> recipientShards;
        RecipientShardContext recipientContext;
        recipientContext.setState(RecipientStateEnum::kCloning);
        recipientShards.emplace_back(ShardId("recipientShard1"), recipientContext);
        recipientShards.emplace_back(ShardId("recipientShard2"), recipientContext);
        coordinatorDoc.setRecipientShards(std::move(recipientShards));

        coordinatorDoc.setCollation(BSON("locale" << "simple"));
        return coordinatorDoc;
    }
};

namespace {

constexpr std::array kAllCoordinatorStates = {
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
};
}  // namespace

TEST_F(ReshardingCoordinatorServiceUtilTest,
       CreateTempCollectionLifecycleRequestProducesInsertAtPreparingToDonateAndDeleteAtCommitting) {
    auto opCtx = makeOperationContext();
    auto coordinatorDoc = makeTempNssTestCoordinatorDoc();

    boost::optional<ChunkVersion> chunkVersion =
        ChunkVersion(CollectionGeneration{OID::gen(), Timestamp(5, 0)}, CollectionPlacement(10, 1));
    BSONObj collation = BSON("locale" << "simple");
    boost::optional<bool> isUnsplittable = true;

    for (auto state : kAllCoordinatorStates) {
        coordinatorDoc.setState(state);
        auto request = createTempCollectionLifecycleRequest(
            opCtx.get(), coordinatorDoc, chunkVersion, collation, isUnsplittable);

        if (state == CoordinatorStateEnum::kPreparingToDonate) {
            ASSERT_TRUE(request.has_value()) << "state=" << idl::serialize(state);
            ASSERT_TRUE(request->getBatchType() == BatchedCommandRequest::BatchType_Insert);

            auto insertDocs = request->getInsertRequest().getDocuments();
            ASSERT_EQ(insertDocs.size(), 1);

            auto expectedDoc =
                resharding::createTempReshardingCollectionType(
                    opCtx.get(), coordinatorDoc, chunkVersion.value(), collation, isUnsplittable)
                    .toBSON();
            auto actualDoc = insertDocs[0];
            ASSERT_BSONELT_EQ(actualDoc["_id"], expectedDoc["_id"]);
            ASSERT_BSONELT_EQ(actualDoc["uuid"], expectedDoc["uuid"]);
            ASSERT_BSONELT_EQ(actualDoc["key"], expectedDoc["key"]);
            ASSERT_BSONELT_EQ(actualDoc["defaultCollation"], expectedDoc["defaultCollation"]);
            ASSERT_BSONELT_EQ(actualDoc["reshardingFields"], expectedDoc["reshardingFields"]);
        } else if (state == CoordinatorStateEnum::kCommitting) {
            ASSERT_TRUE(request.has_value()) << "state=" << idl::serialize(state);
            ASSERT_TRUE(request->getBatchType() == BatchedCommandRequest::BatchType_Delete);
            BSONObj expectedQuery = BSON(
                CollectionType::kNssFieldName << NamespaceStringUtil::serialize(
                    coordinatorDoc.getTempReshardingNss(), SerializationContext::stateDefault()));
            BSONObj expectedDeletes = BSON("q" << expectedQuery << "limit" << 1);
            BSONObj expectedBSON =
                BSON("delete" << NamespaceString::kConfigsvrCollectionsNamespace.coll()
                              << "bypassDocumentValidation" << false << "ordered" << true
                              << "deletes" << BSON_ARRAY(expectedDeletes));
            ASSERT_BSONOBJ_EQ(request->toBSON(), expectedBSON);
        } else {
            // Non-lifecycle states are handled by
            // 'createLegacyTempCollectionReshardingFieldsRequest' and are signaled by boost::none
            // here.
            ASSERT_FALSE(request.has_value()) << "state=" << idl::serialize(state);
        }
    }
}

TEST_F(ReshardingCoordinatorServiceUtilTest,
       CreateLegacyTempCollectionReshardingFieldsRequestProducesUpdatesForNonLifecycleStates) {
    auto opCtx = makeOperationContext();
    auto coordinatorDoc = makeTempNssTestCoordinatorDoc();

    for (auto state : kAllCoordinatorStates) {
        coordinatorDoc.setState(state);
        auto request =
            createLegacyTempCollectionReshardingFieldsRequest(opCtx.get(), coordinatorDoc);

        if (state == CoordinatorStateEnum::kPreparingToDonate ||
            state == CoordinatorStateEnum::kCommitting) {
            // Lifecycle states are handled by 'createTempCollectionLifecycleRequest'.
            ASSERT_FALSE(request.has_value()) << "state=" << idl::serialize(state);
            continue;
        }

        ASSERT_TRUE(request.has_value()) << "state=" << idl::serialize(state);
        ASSERT_TRUE(request->getBatchType() == BatchedCommandRequest::BatchType_Update);
        ASSERT_EQ(request->getUpdateRequest().getUpdates().size(), 1);
        auto updateEntry = request->getUpdateRequest().getUpdates()[0].toBSON();

        BSONObj expectedQuery =
            BSON(CollectionType::kNssFieldName << NamespaceStringUtil::serialize(
                     coordinatorDoc.getTempReshardingNss(), SerializationContext::stateDefault()));
        ASSERT_BSONOBJ_EQ(updateEntry.getObjectField("q"), expectedQuery);

        // The lastmod field has a dynamic value, so remove it for comparison.
        auto setBSON =
            updateEntry.getObjectField("u").getObjectField("$set").removeField("lastmod");

        if (state == CoordinatorStateEnum::kCloning) {
            BSONArrayBuilder donorShardsBuilder;
            for (const auto& donorShard : coordinatorDoc.getDonorShards()) {
                donorShardsBuilder.append(BSON("shardId" << donorShard.getId()));
            }
            BSONObj expectedSet = BSON(
                "reshardingFields.state"
                << idl::serialize(state) << "reshardingFields.recipientFields.approxDocumentsToCopy"
                << coordinatorDoc.getApproxDocumentsToCopy().value()
                << "reshardingFields.recipientFields.approxBytesToCopy"
                << coordinatorDoc.getApproxBytesToCopy().value()
                << "reshardingFields.recipientFields.cloneTimestamp"
                << coordinatorDoc.getCloneTimestamp().value()
                << "reshardingFields.recipientFields.donorShards" << donorShardsBuilder.arr());
            ASSERT_BSONOBJ_EQ(setBSON, expectedSet);
        } else {
            BSONObjBuilder expectedSetBuilder;
            expectedSetBuilder.append("reshardingFields.state", idl::serialize(state));
            if (state == CoordinatorStateEnum::kAborting && coordinatorDoc.getAbortReason()) {
                expectedSetBuilder.append("reshardingFields.abortReason",
                                          *coordinatorDoc.getAbortReason());
                auto abortStatus = resharding::getStatusFromAbortReason(coordinatorDoc);
                expectedSetBuilder.append("reshardingFields.userCanceled",
                                          abortStatus == ErrorCodes::ReshardCollectionAborted);
            }
            ASSERT_BSONOBJ_EQ(setBSON, expectedSetBuilder.obj());
        }
    }
}

TEST_F(ReshardingCoordinatorServiceUtilTest,
       AssertResultIsValidForWriteToConfigCollectionsForTempNssTestDeleteRequest) {
    auto request = BatchedCommandRequest::buildDeleteOp(
        NamespaceString::kConfigsvrCollectionsNamespace,
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
        NamespaceString::kConfigsvrCollectionsNamespace,
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
        NamespaceString::kConfigsvrCollectionsNamespace,
        std::vector<BSONObj>{BSON("key" << 1 << "value" << 2)});
    BSONObj result;

    // We do not need to add anything to the result because we do not validate the result on insert.
    ASSERT_DOES_NOT_THROW(resharding::assertResultIsValidForUpdatesAndDeletes(request, result));
}

TEST_F(ReshardingCoordinatorServiceUtilTest,
       CreateLegacyReshardingFieldsUpdateAddsTelemetryContext) {
    // The reshardingFields persistence path is gated off by featureFlagReshardingInitNoRefresh
    // (which defaults to true). Disable it to exercise the legacy contract this test verifies.
    RAIIServerParameterControllerForTest initNoRefresh{"featureFlagReshardingInitNoRefresh", false};

    auto opCtx = makeOperationContext();

    ReshardingCoordinatorDocument coordinatorDoc;
    auto commonReshardingMetadata = makeMetadata();
    commonReshardingMetadata.setStartTime(Date_t::now());
    commonReshardingMetadata.setPerformVerification(true);

    coordinatorDoc.setCommonReshardingMetadata(std::move(commonReshardingMetadata));

    coordinatorDoc.setState(CoordinatorStateEnum::kInitializing);

    // Set a telemetry context
    auto telemetryContext = otel::traces::Span::createTelemetryContext();
    coordinatorDoc.setTelemetryContext(
        otel::traces::TelemetryContextSerializer::toBSON(telemetryContext));

    auto updateBSON = createLegacyReshardingFieldsUpdate(opCtx.get(), coordinatorDoc);

    auto setFields = updateBSON.getObjectField("$set");
    auto reshardingFields = setFields.getObjectField("reshardingFields");
    ASSERT(reshardingFields.hasField("telemetryContext"));
}

TEST_F(ReshardingCoordinatorServiceUtilTest,
       SkipReshardingFieldsWritesForCoordinatorRespectsInitNoRefreshFlag) {
    ReshardingCoordinatorDocument coordinatorDoc;
    auto metadata = makeMetadata();
    metadata.setStartTime(Date_t::now());
    coordinatorDoc.setCommonReshardingMetadata(std::move(metadata));
    coordinatorDoc.setState(CoordinatorStateEnum::kInitializing);

    ASSERT_TRUE(skipReshardingFieldsWritesForCoordinator(coordinatorDoc));

    {
        RAIIServerParameterControllerForTest flagScope{"featureFlagReshardingInitNoRefresh", false};
        ASSERT_FALSE(skipReshardingFieldsWritesForCoordinator(coordinatorDoc));
    }
}

TEST_F(
    ReshardingCoordinatorServiceUtilTest,
    CreateReshardedCollectionEntryUpdateAlwaysWritesIdentityFieldsRegardlessOfInitNoRefreshFlag) {
    auto runWithFlag = [&](bool initNoRefreshOn) {
        RAIIServerParameterControllerForTest flagScope{"featureFlagReshardingInitNoRefresh",
                                                       initNoRefreshOn};

        auto opCtx = makeOperationContext();
        ReshardingCoordinatorDocument coordinatorDoc;
        auto metadata = makeMetadata();
        metadata.setStartTime(Date_t::now());
        coordinatorDoc.setCommonReshardingMetadata(std::move(metadata));
        coordinatorDoc.setState(CoordinatorStateEnum::kCommitting);

        auto updateBSON = createReshardedCollectionEntryUpdate(
            opCtx.get(), coordinatorDoc, OID::gen(), Timestamp(1, 2));
        auto setFields = updateBSON.getObjectField("$set");
        ASSERT(setFields.hasField("uuid"))
            << "initNoRefreshOn=" << initNoRefreshOn << " update=" << updateBSON;
        ASSERT(setFields.hasField("key"))
            << "initNoRefreshOn=" << initNoRefreshOn << " update=" << updateBSON;
        ASSERT(setFields.hasField("lastmodEpoch"))
            << "initNoRefreshOn=" << initNoRefreshOn << " update=" << updateBSON;
        ASSERT(setFields.hasField("timestamp"))
            << "initNoRefreshOn=" << initNoRefreshOn << " update=" << updateBSON;
        ASSERT_FALSE(setFields.hasField("reshardingFields.state"))
            << "initNoRefreshOn=" << initNoRefreshOn << " update=" << updateBSON;
        ASSERT_FALSE(setFields.hasField("reshardingFields.recipientFields"))
            << "initNoRefreshOn=" << initNoRefreshOn << " update=" << updateBSON;
    };
    runWithFlag(true);
    runWithFlag(false);
}

TEST_F(ReshardingCoordinatorServiceUtilTest,
       CreateTempReshardingCollectionTypeOmitsReshardingFieldsWhenInitNoRefreshFlagOn) {
    auto opCtx = makeOperationContext();
    ReshardingCoordinatorDocument coordinatorDoc;
    auto metadata = makeMetadata();
    metadata.setStartTime(Date_t::now());
    coordinatorDoc.setCommonReshardingMetadata(std::move(metadata));
    coordinatorDoc.setState(CoordinatorStateEnum::kPreparingToDonate);

    ChunkVersion chunkVersion{CollectionGeneration{OID::gen(), Timestamp(5, 0)},
                              CollectionPlacement(10, 1)};
    BSONObj collation = BSON("locale" << "simple");

    auto collType = createTempReshardingCollectionType(
        opCtx.get(), coordinatorDoc, chunkVersion, collation, /*isUnsplittable*/ boost::none);

    ASSERT_FALSE(collType.getReshardingFields().has_value());
}

TEST_F(ReshardingCoordinatorServiceUtilTest,
       SkipReshardingFieldsWritesForCoordinatorHonorsPinnedLastLTSVersionContext) {
    ReshardingCoordinatorDocument coordinatorDoc;
    auto metadata = makeMetadata();
    metadata.setStartTime(Date_t::now());

    ForwardableOperationMetadata fom;
    // (Generic FCV reference): pin the operation to last-LTS so the InitNoRefresh feature flag,
    // which is gated on the latest FCV, evaluates to false and the legacy write path is
    // selected -- regardless of the global flag value.
    fom.setVersionContext(
        VersionContext{ServerGlobalParams::FCVSnapshot{multiversion::GenericFCV::kLastLTS}});
    metadata.setForwardableOpMetadata(std::move(fom));
    coordinatorDoc.setCommonReshardingMetadata(std::move(metadata));
    coordinatorDoc.setState(CoordinatorStateEnum::kInitializing);

    ASSERT_FALSE(skipReshardingFieldsWritesForCoordinator(coordinatorDoc));
}

TEST_F(ReshardingCoordinatorServiceUtilTest,
       SkipReshardingFieldsWritesForCoordinatorHonorsGlobalFlagDisableShortCircuit) {
    RAIIServerParameterControllerForTest flagScope{"featureFlagReshardingInitNoRefresh", false};

    ReshardingCoordinatorDocument coordinatorDoc;
    auto metadata = makeMetadata();
    metadata.setStartTime(Date_t::now());

    ForwardableOperationMetadata fom;
    // (Generic FCV reference): pin the operation to latest so the InitNoRefresh flag would
    // normally be on; the test then forces the global flag off to verify the short-circuit.
    fom.setVersionContext(
        VersionContext{ServerGlobalParams::FCVSnapshot{multiversion::GenericFCV::kLatest}});
    metadata.setForwardableOpMetadata(std::move(fom));
    coordinatorDoc.setCommonReshardingMetadata(std::move(metadata));
    coordinatorDoc.setState(CoordinatorStateEnum::kInitializing);

    ASSERT_FALSE(skipReshardingFieldsWritesForCoordinator(coordinatorDoc));
}

TEST_F(ReshardingCoordinatorServiceUtilTest, RegistryPathThrowsWhenReshardingUUIDNotFound) {
    auto opCtx = makeOperationContext();

    ASSERT_THROWS_CODE(retrieveReshardingUUID(opCtx.get(), kSourceNss),
                       DBException,
                       ErrorCodes::NoSuchReshardCollection);
}

TEST_F(ReshardingCoordinatorServiceUtilTest, RegistryPathReturnsReshardingUUID) {
    auto opCtx = makeOperationContext();
    auto reshardingUUID = UUID::gen();
    auto meta = makeMetadata(reshardingUUID);
    LocalReshardingOperationsRegistry::get().registerOperation(
        LocalReshardingOperationsRegistry::Role::kCoordinator, meta);

    ASSERT_EQ(retrieveReshardingUUID(opCtx.get(), kSourceNss), reshardingUUID);
}

/**
 * Parameterized fixture exercising the per-provenance behavior of
 * createLegacyReshardingFieldsUpdate and createTempReshardingCollectionType.
 */
class ReshardingCoordinatorServiceUtilProvenanceTest
    : public ReshardingCoordinatorServiceUtilTest,
      public ::testing::WithParamInterface<ReshardingProvenanceEnum> {
protected:
    ReshardingCoordinatorDocument makeCoordinatorDocWithProvenance(CoordinatorStateEnum state) {
        ReshardingCoordinatorDocument doc;
        auto metadata = makeMetadata();
        metadata.setStartTime(Date_t::now());
        metadata.setProvenance(GetParam());
        doc.setCommonReshardingMetadata(std::move(metadata));
        doc.setState(state);

        DonorShardContext donorCtx;
        donorCtx.setState(DonorStateEnum::kPreparingToDonate);
        doc.setDonorShards({DonorShardEntry(ShardId("donor0"), donorCtx)});

        RecipientShardContext recipientCtx;
        recipientCtx.setState(RecipientStateEnum::kUnused);
        doc.setRecipientShards({RecipientShardEntry(ShardId("recipient0"), recipientCtx)});

        return doc;
    }
};

INSTANTIATE_TEST_SUITE_P(Provenance,
                         ReshardingCoordinatorServiceUtilProvenanceTest,
                         ::testing::Values(ReshardingProvenanceEnum::kReshardCollection,
                                           ReshardingProvenanceEnum::kMoveCollection,
                                           ReshardingProvenanceEnum::kUnshardCollection,
                                           ReshardingProvenanceEnum::kRewriteCollection),
                         [](const ::testing::TestParamInfo<ReshardingProvenanceEnum>& info) {
                             return std::string(idl::serialize(info.param));
                         });

TEST_P(ReshardingCoordinatorServiceUtilProvenanceTest,
       CollectionUpdateAtCommitSetsUnsplittableForUnshardOnly) {
    auto opCtx = makeOperationContext();
    auto doc = makeCoordinatorDocWithProvenance(CoordinatorStateEnum::kCommitting);

    auto update =
        createReshardedCollectionEntryUpdate(opCtx.get(), doc, OID::gen(), Timestamp(1, 2));
    auto setFields = update.getObjectField("$set");

    if (isUnshardCollection(GetParam())) {
        ASSERT_TRUE(setFields.hasField("unsplittable"));
        ASSERT_TRUE(setFields["unsplittable"].Bool());
    } else {
        ASSERT_FALSE(setFields.hasField("unsplittable"));
    }
}

TEST_P(ReshardingCoordinatorServiceUtilProvenanceTest,
       TempCollectionBlocksMigrationsForReshardAndRewriteOnly) {
    auto opCtx = makeOperationContext();
    auto doc = makeCoordinatorDocWithProvenance(CoordinatorStateEnum::kPreparingToDonate);
    ChunkVersion chunkVersion(CollectionGeneration{OID::gen(), Timestamp(5, 0)},
                              CollectionPlacement(10, 1));

    auto collType = createTempReshardingCollectionType(
        opCtx.get(), doc, chunkVersion, BSONObj() /* collation */, boost::none);

    const bool expectMigrationsBlocked =
        isOrdinaryReshardCollection(GetParam()) || isRewriteCollection(GetParam());
    ASSERT_EQ(collType.getAllowMigrations(), !expectMigrationsBlocked);
}

TEST_P(ReshardingCoordinatorServiceUtilProvenanceTest,
       CollectionUpdateAtInitializingCopiesProvenance) {
    auto opCtx = makeOperationContext();
    auto doc = makeCoordinatorDocWithProvenance(CoordinatorStateEnum::kInitializing);

    auto update = createLegacyReshardingFieldsUpdate(opCtx.get(), doc);

    auto reshardingFields = update.getObjectField("$set").getObjectField("reshardingFields");
    ASSERT_EQ(reshardingFields.getStringField("provenance"), idl::serialize(GetParam()));
}

TEST_P(ReshardingCoordinatorServiceUtilProvenanceTest, TempCollectionTypeCopiesProvenance) {
    // The temp-collection reshardingFields subtree is gated off by
    // featureFlagReshardingInitNoRefresh (which defaults to true).
    RAIIServerParameterControllerForTest initNoRefresh{"featureFlagReshardingInitNoRefresh", false};

    auto opCtx = makeOperationContext();
    auto doc = makeCoordinatorDocWithProvenance(CoordinatorStateEnum::kPreparingToDonate);
    ChunkVersion chunkVersion(CollectionGeneration{OID::gen(), Timestamp(5, 0)},
                              CollectionPlacement(10, 1));

    auto collType =
        createTempReshardingCollectionType(opCtx.get(), doc, chunkVersion, BSONObj(), boost::none);

    ASSERT_TRUE(collType.getReshardingFields().has_value());
    ASSERT_EQ(collType.getReshardingFields()->getProvenance(), GetParam());
}

TEST_F(ReshardingCoordinatorServiceUtilTest,
       ComputeVerificationDeadlineAtCriticalSectionStartMatchesShareOfTotalBudget) {
    RAIIServerParameterControllerForTest percentCtrl{
        "reshardingVerificationDeltaWaitRemainingCriticalSectionPercent", 30};

    const auto expiresAt = Date_t::fromMillisSinceEpoch(1'000'000);
    const auto reachedStrictConsistencyTime = expiresAt - Milliseconds(100'000);
    ReshardingCoordinatorDocument coordinatorDoc;
    coordinatorDoc.setCommonReshardingMetadata(makeMetadata());
    coordinatorDoc.setCriticalSectionExpiresAt(expiresAt);

    ASSERT_EQ(computeVerificationDeadline(coordinatorDoc, reachedStrictConsistencyTime),
              expiresAt - Milliseconds(70'000));
}

TEST_F(ReshardingCoordinatorServiceUtilTest,
       ComputeVerificationDeadlineEqualsExpiresAtAt100Percent) {
    // 100% leaves no reserve: the wait may run right up to critical-section expiry.
    RAIIServerParameterControllerForTest percentCtrl{
        "reshardingVerificationDeltaWaitRemainingCriticalSectionPercent", 100};

    const auto expiresAt = Date_t::fromMillisSinceEpoch(1'000'000);
    const auto reachedStrictConsistencyTime = expiresAt - Milliseconds(40'000);
    ReshardingCoordinatorDocument coordinatorDoc;
    coordinatorDoc.setCommonReshardingMetadata(makeMetadata());
    coordinatorDoc.setCriticalSectionExpiresAt(expiresAt);

    ASSERT_EQ(computeVerificationDeadline(coordinatorDoc, reachedStrictConsistencyTime), expiresAt);
}

TEST_F(ReshardingCoordinatorServiceUtilTest, ComputeVerificationDeadlineEqualsReachedAtAt0Percent) {
    // 0% grants no time: the wait is skipped immediately.
    RAIIServerParameterControllerForTest percentCtrl{
        "reshardingVerificationDeltaWaitRemainingCriticalSectionPercent", 0};

    const auto expiresAt = Date_t::fromMillisSinceEpoch(1'000'000);
    const auto reachedStrictConsistencyTime = expiresAt - Milliseconds(40'000);
    ReshardingCoordinatorDocument coordinatorDoc;
    coordinatorDoc.setCommonReshardingMetadata(makeMetadata());
    coordinatorDoc.setCriticalSectionExpiresAt(expiresAt);

    ASSERT_EQ(computeVerificationDeadline(coordinatorDoc, reachedStrictConsistencyTime),
              reachedStrictConsistencyTime);
}

TEST_F(ReshardingCoordinatorServiceUtilTest,
       ComputeVerificationDeadlineClampsToReachedAtWhenNoBudgetRemains) {
    // Strict consistency reached after the critical section already expired: remaining < 0.
    // The deadline must clamp to reachedStrictConsistencyTime (immediate skip).
    RAIIServerParameterControllerForTest percentCtrl{
        "reshardingVerificationDeltaWaitRemainingCriticalSectionPercent", 80};

    const auto expiresAt = Date_t::fromMillisSinceEpoch(1'000'000);
    const auto reachedStrictConsistencyTime = expiresAt + Milliseconds(5'000);
    ReshardingCoordinatorDocument coordinatorDoc;
    coordinatorDoc.setCommonReshardingMetadata(makeMetadata());
    coordinatorDoc.setCriticalSectionExpiresAt(expiresAt);

    ASSERT_EQ(computeVerificationDeadline(coordinatorDoc, reachedStrictConsistencyTime),
              reachedStrictConsistencyTime);
}

}  // namespace resharding
}  // namespace mongo
