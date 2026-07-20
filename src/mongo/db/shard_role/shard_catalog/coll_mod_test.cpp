// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/coll_mod.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/client.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/rss/stub_persistence_provider.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_role/ddl/coll_mod_gen.h"
#include "mongo/db/shard_role/ddl/create_gen.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/backwards_compatible_collection_options_util.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/db/shard_role/shard_catalog/durable_catalog.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/mdb_catalog.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/timeseries_collmod.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/timeseries_test_util.h"
#include "mongo/db/topology/vector_clock/vector_clock_mutable.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {
TEST(CollModOptionTest, isConvertingIndexToUnique) {
    IDLParserContext ctx("collMod");
    auto requestObj = fromjson("{index: {keyPattern: {a: 1}, unique: true}}");
    auto request = CollModRequest::parse(requestObj, ctx);
    ASSERT_TRUE(isCollModIndexUniqueConversion(request));

    requestObj = fromjson("{index: {keyPattern: {a: 1}, unique: true, hidden: true}}");
    request = CollModRequest::parse(requestObj, ctx);
    ASSERT_TRUE(isCollModIndexUniqueConversion(request));

    requestObj = fromjson(
        "{index: {keyPattern: {a: 1}, unique: true, hidden: true}, validationAction: 'warn'}");
    request = CollModRequest::parse(requestObj, ctx);
    ASSERT_TRUE(isCollModIndexUniqueConversion(request));

    requestObj = fromjson("{index: {keyPattern: {a: 1}, unique: true}, dryRun: true}");
    request = CollModRequest::parse(requestObj, ctx);
    ASSERT_FALSE(isCollModIndexUniqueConversion(request));

    requestObj = fromjson("{index: {keyPattern: {a: 1}, unique: true}, dryRun: false}");
    request = CollModRequest::parse(requestObj, ctx);
    ASSERT_TRUE(isCollModIndexUniqueConversion(request));

    requestObj = fromjson("{index: {keyPattern: {a: 1}, prepareUnique: true}}");
    request = CollModRequest::parse(requestObj, ctx);
    ASSERT_FALSE(isCollModIndexUniqueConversion(request));

    requestObj = fromjson("{validationAction: 'warn'}");
    request = CollModRequest::parse(requestObj, ctx);
    ASSERT_FALSE(isCollModIndexUniqueConversion(request));
}

TEST(CollModOptionTest, makeDryRunRequest) {
    IDLParserContext ctx("collMod");
    auto requestObj = fromjson("{index: {keyPattern: {a: 1}, unique: true}}");
    auto request = CollModRequest::parse(requestObj, ctx);
    auto dryRunRequest = makeCollModDryRunRequest(request);
    ASSERT_TRUE(dryRunRequest.getIndex()->getKeyPattern()->binaryEqual(fromjson("{a: 1}")));
    ASSERT_TRUE(dryRunRequest.getIndex()->getUnique() && *dryRunRequest.getIndex()->getUnique());
    ASSERT_TRUE(dryRunRequest.getDryRun() && *dryRunRequest.getDryRun());

    requestObj = fromjson("{index: {keyPattern: {a: 1}, unique: true, hidden: true}}");
    request = CollModRequest::parse(requestObj, ctx);
    dryRunRequest = makeCollModDryRunRequest(request);
    ASSERT_TRUE(dryRunRequest.getIndex()->getKeyPattern()->binaryEqual(fromjson("{a: 1}")));
    ASSERT_TRUE(dryRunRequest.getIndex()->getUnique() && *dryRunRequest.getIndex()->getUnique());
    ASSERT_FALSE(dryRunRequest.getIndex()->getHidden());
    ASSERT_TRUE(dryRunRequest.getDryRun() && *dryRunRequest.getDryRun());

    requestObj = fromjson(
        "{index: {keyPattern: {a: 1}, unique: true, hidden: true}, validationAction: 'warn'}");
    request = CollModRequest::parse(requestObj, ctx);
    dryRunRequest = makeCollModDryRunRequest(request);
    ASSERT_TRUE(dryRunRequest.getIndex()->getKeyPattern()->binaryEqual(fromjson("{a: 1}")));
    ASSERT_TRUE(dryRunRequest.getIndex()->getUnique() && *dryRunRequest.getIndex()->getUnique());
    ASSERT_FALSE(dryRunRequest.getIndex()->getHidden());
    ASSERT_FALSE(dryRunRequest.getValidationAction());
    ASSERT_TRUE(dryRunRequest.getDryRun() && *dryRunRequest.getDryRun());

    requestObj = fromjson("{index: {keyPattern: {a: 1}, unique: true}, dryRun: false}");
    request = CollModRequest::parse(requestObj, ctx);
    dryRunRequest = makeCollModDryRunRequest(request);
    ASSERT_TRUE(dryRunRequest.getIndex()->getKeyPattern()->binaryEqual(fromjson("{a: 1}")));
    ASSERT_TRUE(dryRunRequest.getIndex()->getUnique() && *dryRunRequest.getIndex()->getUnique());
    ASSERT_TRUE(dryRunRequest.getDryRun() && *dryRunRequest.getDryRun());
}

class CollModTest : public ServiceContextMongoDTest {
protected:
    explicit CollModTest(Options options = {}) : ServiceContextMongoDTest(std::move(options)) {}

    void setUp() override {
        // Set up mongod.
        ServiceContextMongoDTest::setUp();

        auto service = getServiceContext();

        // Set up ReplicationCoordinator and ensure that we are primary.
        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(service);
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
        repl::ReplicationCoordinator::set(service, std::move(replCoord));
    }
    void tearDown() override {
        // Tear down mongod.
        ServiceContextMongoDTest::tearDown();
    }
};

ServiceContext::UniqueOperationContext makeOpCtx() {
    auto opCtx = cc().makeOperationContext();
    repl::createOplog(opCtx.get());
    return opCtx;
}

CollectionAcquisition acquireCollForRead(OperationContext* opCtx, const NamespaceString& nss) {
    return acquireCollection(
        opCtx,
        CollectionAcquisitionRequest(nss,
                                     PlacementConcern(boost::none, ShardVersion::UNTRACKED()),
                                     repl::ReadConcernArgs::get(opCtx),
                                     AcquisitionPrerequisites::kRead),
        MODE_IS);
}

// When collMod changes bucketing parameters, fixedBucketing must be automatically set to false.
TEST_F(CollModTest, CollModDisablesFixedBucketingOnBucketingParameterChange) {
    unittest::ServerParameterGuard fixedBucketingFlagController("featureFlagFixedBucketingCatalog",
                                                                true);
    unittest::ServerParameterGuard viewlessFlagController(
        "featureFlagCreateViewlessTimeseriesCollections", true);

    NamespaceString curNss = NamespaceString::createNamespaceString_forTest("test.curColl");
    auto opCtx = makeOpCtx();

    // Create a viewless timeseries collection with fixedBucketing: true.
    TimeseriesOptions tsOptions("t");
    tsOptions.setBucketMaxSpanSeconds(100);
    tsOptions.setBucketRoundingSeconds(100);
    tsOptions.setFixedBucketing(true);
    CreateCommand createCmd(curNss);
    createCmd.getCreateCollectionRequest().setTimeseries(std::move(tsOptions));
    uassertStatusOK(createCollection(opCtx.get(), createCmd));

    // Verify fixedBucketing was persisted as true.
    auto tsNss = timeseries::test_util::resolveTimeseriesNss(curNss);
    {
        const auto coll = acquireCollForRead(opCtx.get(), tsNss);
        ASSERT_TRUE(
            coll.getCollectionPtr()->getTimeseriesOptions()->getFixedBucketing().has_value());
        ASSERT_TRUE(coll.getCollectionPtr()->getTimeseriesOptions()->getFixedBucketing());
    }

    // Run collMod that changes the bucketing parameters.
    CollMod collModCmd(curNss);
    CollModTimeseries collModTs;
    collModTs.setBucketMaxSpanSeconds(200);
    collModTs.setBucketRoundingSeconds(200);
    collModCmd.setTimeseries(std::move(collModTs));
    BSONObjBuilder result;
    uassertStatusOK(timeseries::processCollModCommandWithTimeSeriesTranslation(
        opCtx.get(), curNss, collModCmd, true, &result));

    // fixedBucketing must have been set to false because bucketing params changed.
    {
        const auto coll = acquireCollForRead(opCtx.get(), tsNss);
        ASSERT_TRUE(
            coll.getCollectionPtr()->getTimeseriesOptions()->getFixedBucketing().has_value());
        ASSERT_FALSE(coll.getCollectionPtr()->getTimeseriesOptions()->getFixedBucketing());
    }
}

// When collMod specifies the same bucketing parameter values as the current ones, fixedBucketing
// must remain unchanged (no spurious disable).
TEST_F(CollModTest, CollModFixedBucketingNoOpWhenBucketingParamsUnchanged) {
    unittest::ServerParameterGuard fixedBucketingFlagController("featureFlagFixedBucketingCatalog",
                                                                true);
    unittest::ServerParameterGuard viewlessFlagController(
        "featureFlagCreateViewlessTimeseriesCollections", true);

    NamespaceString curNss = NamespaceString::createNamespaceString_forTest("test.curColl");
    auto opCtx = makeOpCtx();

    // Create a viewless timeseries collection with fixedBucketing: true.
    TimeseriesOptions tsOptions("t");
    tsOptions.setBucketMaxSpanSeconds(100);
    tsOptions.setBucketRoundingSeconds(100);
    tsOptions.setFixedBucketing(true);
    CreateCommand createCmd(curNss);
    createCmd.getCreateCollectionRequest().setTimeseries(std::move(tsOptions));
    uassertStatusOK(createCollection(opCtx.get(), createCmd));

    // Run collMod with the same bucketing parameter values.
    CollMod collModCmd(curNss);
    CollModTimeseries collModTs;
    collModTs.setBucketMaxSpanSeconds(100);
    collModTs.setBucketRoundingSeconds(100);
    collModCmd.setTimeseries(std::move(collModTs));
    BSONObjBuilder result;
    uassertStatusOK(timeseries::processCollModCommandWithTimeSeriesTranslation(
        opCtx.get(), curNss, collModCmd, true, &result));

    // No parameter actually changed: fixedBucketing must remain true.
    auto tsNss = timeseries::test_util::resolveTimeseriesNss(curNss);
    const auto coll = acquireCollForRead(opCtx.get(), tsNss);
    ASSERT_TRUE(coll.getCollectionPtr()->getTimeseriesOptions()->getFixedBucketing().has_value());
    ASSERT_TRUE(coll.getCollectionPtr()->getTimeseriesOptions()->getFixedBucketing());
}

TEST_F(CollModTest, TimeseriesLegacyBucketingParameterChangedRemoval) {
    NamespaceString curNss = NamespaceString::createNamespaceString_forTest("test.curColl");

    auto opCtx = makeOpCtx();
    CreateCommand cmd = CreateCommand(curNss);
    cmd.setTimeseries(TimeseriesOptions("t"));
    uassertStatusOK(createCollection(opCtx.get(), cmd));
    auto tsNss = timeseries::test_util::resolveTimeseriesNss(curNss);

    auto catalogId = CollectionCatalog::get(opCtx.get())
                         ->lookupCollectionByNamespace(opCtx.get(), tsNss)
                         ->getCatalogId();
    auto mdbCatalog = MDBCatalog::get(opCtx.get());

    // Set the `md.timeseriesBucketingParametersHaveChanged` field through the durable catalog
    // (as this option is deprecated and can't be set anymore through other means).
    {
        WriteUnitOfWork wuow{opCtx.get()};
        auto catalogEntry =
            durable_catalog::getParsedCatalogEntry(opCtx.get(), catalogId, mdbCatalog);
        catalogEntry->metadata->timeseriesBucketingParametersHaveChanged_DO_NOT_USE = false;
        durable_catalog::putMetaData(opCtx.get(), catalogId, *catalogEntry->metadata, mdbCatalog);
        wuow.commit();
    }

    // The command must be sent to the buckets collection directly (no time-series translation).
    // This is acceptable since this is an internal parameter.
    CollMod collModCmd(tsNss);
    collModCmd.set_removeLegacyTimeseriesBucketingParametersHaveChanged(true);
    BSONObjBuilder result;
    uassertStatusOK(processCollModCommand(opCtx.get(), tsNss, collModCmd, nullptr, &result));

    {
        auto catalogEntry =
            durable_catalog::getParsedCatalogEntry(opCtx.get(), catalogId, mdbCatalog);
        ASSERT_FALSE(catalogEntry->metadata->timeseriesBucketingParametersHaveChanged_DO_NOT_USE);
    }
}

TEST_F(CollModTest, CollModTimeseriesMixedSchemaData) {
    NamespaceString curNss = NamespaceString::createNamespaceString_forTest("test.curColl");

    auto opCtx = makeOpCtx();
    auto tsOptions = TimeseriesOptions("t");
    CreateCommand cmd = CreateCommand(curNss);
    cmd.getCreateCollectionRequest().setTimeseries(std::move(tsOptions));
    uassertStatusOK(createCollection(opCtx.get(), cmd));
    auto tsNss = timeseries::test_util::resolveTimeseriesNss(curNss);

    CollMod collModCmd(curNss);
    CollModRequest collModRequest;
    collModRequest.setTimeseriesBucketsMayHaveMixedSchemaData(true);
    collModCmd.setCollModRequest(collModRequest);
    BSONObjBuilder result;
    uassertStatusOK(timeseries::processCollModCommandWithTimeSeriesTranslation(
        opCtx.get(), curNss, collModCmd, true, &result));
    {
        const auto tsCollForRead = acquireCollForRead(opCtx.get(), tsNss);
        auto mixedSchemaState =
            tsCollForRead.getCollectionPtr()->getTimeseriesMixedSchemaBucketsState();
        ASSERT_TRUE(mixedSchemaState.isValid());
        ASSERT_TRUE(mixedSchemaState.mustConsiderMixedSchemaBucketsInReads());
        ASSERT_TRUE(mixedSchemaState.canStoreMixedSchemaBucketsSafely());
    }

    // Test that both backwards compatibles option and legacy parameter have been properly set
    auto coll =
        CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespace(opCtx.get(), tsNss);
    auto catalogEntry = durable_catalog::getParsedCatalogEntry(
        opCtx.get(), coll->getCatalogId(), MDBCatalog::get(opCtx.get()));
    auto metadata = catalogEntry->metadata;
    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    boost::optional<bool> optBackwardsCompatibleFlag = storageEngine->getFlagFromStorageOptions(
        metadata->options.storageEngine,
        backwards_compatible_collection_options::kTimeseriesBucketsMayHaveMixedSchemaData);
    ASSERT_TRUE(optBackwardsCompatibleFlag);
    ASSERT_TRUE(*optBackwardsCompatibleFlag);
    ASSERT_TRUE(metadata->timeseriesBucketsMayHaveMixedSchemaData);
    ASSERT_TRUE(*metadata->timeseriesBucketsMayHaveMixedSchemaData);
}

class CollModTimestampedTest : public CollModTest {
public:
    // Disable table logging. When table logging is enabled, timestamps are discarded by WiredTiger.
    CollModTimestampedTest() : CollModTest(Options{}.forceDisableTableLogging()) {}
};

// Regression test for SERVER-104640. Test that the MixedSchema timeseries flag can be read at a
// point-in-time from the collection catalog.
TEST_F(CollModTimestampedTest, CollModTimeseriesMixedSchemaFlagPointInTimeLookup) {
    NamespaceString curNss = NamespaceString::createNamespaceString_forTest("test.curColl");

    auto opCtx = makeOpCtx();

    CreateCommand cmd = CreateCommand(curNss);
    cmd.getCreateCollectionRequest().setTimeseries(TimeseriesOptions("t"));
    uassertStatusOK(createCollection(opCtx.get(), cmd));
    auto tsNss = timeseries::test_util::resolveTimeseriesNss(curNss);

    auto collModTime = VectorClockMutable::get(opCtx.get())->tickClusterTime(1).asTimestamp();
    shard_role_details::getRecoveryUnit(opCtx.get())->setCommitTimestamp(collModTime);

    CollMod collModCmd(curNss);
    collModCmd.setTimeseriesBucketsMayHaveMixedSchemaData(true);
    CollModTimeseries collModTs;
    collModTs.setBucketMaxSpanSeconds(20000);
    collModTs.setBucketRoundingSeconds(20000);
    collModCmd.setTimeseries(std::move(collModTs));
    BSONObjBuilder result;
    uassertStatusOK(timeseries::processCollModCommandWithTimeSeriesTranslation(
        opCtx.get(), curNss, collModCmd, true, &result));

    // Check the collection at the timestamp of the collMod
    {
        ReadSourceScope scope(opCtx.get(), RecoveryUnit::ReadSource::kProvided, collModTime);
        auto collAfter = CollectionCatalog::get(opCtx.get())
                             ->establishConsistentCollection(opCtx.get(), tsNss, collModTime);
        ASSERT_TRUE(collAfter->getTimeseriesMixedSchemaBucketsState()
                        .mustConsiderMixedSchemaBucketsInReads());
        ASSERT_TRUE(
            collAfter->getTimeseriesMixedSchemaBucketsState().canStoreMixedSchemaBucketsSafely());
    }

    // Check the collection at a timestamp before the collMod
    {
        ReadSourceScope scope(opCtx.get(), RecoveryUnit::ReadSource::kProvided, collModTime - 1);
        auto collBefore = CollectionCatalog::get(opCtx.get())
                              ->establishConsistentCollection(opCtx.get(), tsNss, collModTime - 1);
        ASSERT_FALSE(collBefore->getTimeseriesMixedSchemaBucketsState()
                         .mustConsiderMixedSchemaBucketsInReads());
        ASSERT_FALSE(
            collBefore->getTimeseriesMixedSchemaBucketsState().canStoreMixedSchemaBucketsSafely());
    }
}

bool areRecordIdsReplicated(OperationContext* opCtx, const NamespaceString& nss) {
    const auto coll = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest(nss,
                                     PlacementConcern(boost::none, ShardVersion::UNTRACKED()),
                                     repl::ReadConcernArgs::get(opCtx),
                                     AcquisitionPrerequisites::kRead),
        MODE_IS);

    ASSERT_TRUE(coll.exists()) << "Unable to get collection options for "
                               << nss.toStringForErrorMsg()
                               << " because collection does not exist.";

    return coll.getCollectionPtr()->areRecordIdsReplicated();
}

TEST_F(CollModTest, CollModSetting_ReplicatedRecordIds_ToFalse_Succeeds) {
    auto opCtx = makeOpCtx();
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.collModColl");

    // Enabling the Replicated RecordId flag
    unittest::ServerParameterGuard featureFlagRecordIdsReplicatedController(
        "featureFlagRecordIdsReplicated", true);

    // Creating the collection, it will have replicated record Ids since the feature flag is on.
    CreateCommand cmd = CreateCommand(nss);
    uassertStatusOK(createCollection(opCtx.get(), cmd));

    // Confirm it has replicated record Ids
    ASSERT_TRUE(areRecordIdsReplicated(opCtx.get(), nss));

    // Modify it disabling replicated record Ids
    CollMod collModCmd(nss);
    collModCmd.setRecordIdsReplicated(false);
    BSONObjBuilder result;
    uassertStatusOK(processCollModCommand(opCtx.get(), nss, collModCmd, nullptr, &result));

    // Confirm replicated record Ids have been disabled
    ASSERT_FALSE(areRecordIdsReplicated(opCtx.get(), nss));
}

TEST_F(CollModTest, CollModSetting_ReplicatedRecordIds_ToTrue_Fails) {
    auto opCtx = makeOpCtx();
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.collModColl");

    // Creating the collection, it will Not have replicated record Ids since the feature
    // flag is not enabled.
    CreateCommand cmd = CreateCommand(nss);
    uassertStatusOK(createCollection(opCtx.get(), cmd));

    // Confirm it doesn't have replicated record Ids
    ASSERT_FALSE(areRecordIdsReplicated(opCtx.get(), nss));

    // Attempt to modify it disabling replicated record Ids
    CollMod collModCmd(nss);
    collModCmd.setRecordIdsReplicated(true);
    BSONObjBuilder result;
    // collMod should fail.
    ASSERT_EQ(ErrorCodes::InvalidOptions,
              processCollModCommand(opCtx.get(), nss, collModCmd, nullptr, &result).code());

    // Confirm it still doesn't have replicated record Ids
    ASSERT_FALSE(areRecordIdsReplicated(opCtx.get(), nss));
}

class StubPersistenceProviderRequiringReplicatedRecordIds : public rss::StubPersistenceProvider {
public:
    std::string name() const override {
        return "MockPersistenceProviderRequiringReplicatedRecordIds";
    }

    bool shouldUseReplicatedRecordIds() const override {
        return true;
    }

    bool shouldUseReplicatedCatalogIdentifiers() const override {
        return false;
    }

    bool supportsTableLogging() const override {
        return true;
    }

    const char* getWTMemoryPageMaxForOplogStrValue() const override {
        return "10m";  // 10MB
    }

    bool supportsUnstableCheckpoints() const override {
        return true;
    }

    bool shouldUseOplogWritesForFlowControlSampling() const override {
        return true;
    }

    bool supportsAsyncOplogMarkerGeneration() const override {
        return false;
    }

    bool supportsOplogSampling() const override {
        return false;
    }

    bool supportsPersistentOplogCapMaintainerThread() const override {
        return true;
    }

    bool shouldUseReplicatedFastCount() const override {
        return false;
    }
};

TEST_F(CollModTest, CollModSetting_ReplicatedRecordIds_ToFalse_WhenProviderRequiresIt_Fails) {
    rss::ReplicatedStorageService::get(getServiceContext())
        .setPersistenceProvider(
            std::make_unique<StubPersistenceProviderRequiringReplicatedRecordIds>());

    auto opCtx = makeOpCtx();
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.collModColl");

    // Enabling the Replicated RecordId flag
    unittest::ServerParameterGuard featureFlagRecordIdsReplicatedController(
        "featureFlagRecordIdsReplicated", true);

    // Creating the collection, it will have replicated record Ids since the feature flag is on.
    CreateCommand cmd = CreateCommand(nss);
    uassertStatusOK(createCollection(opCtx.get(), cmd));

    // Confirm it has replicated record Ids
    ASSERT_TRUE(areRecordIdsReplicated(opCtx.get(), nss));

    // Attempt to modify it disabling replicated record Ids
    CollMod collModCmd(nss);
    collModCmd.setRecordIdsReplicated(false);
    BSONObjBuilder result;
    // collMod should fail.
    ASSERT_EQ(ErrorCodes::InvalidOptions,
              processCollModCommand(opCtx.get(), nss, collModCmd, nullptr, &result).code());

    // Confirm it still have replicated record Ids
    ASSERT_TRUE(areRecordIdsReplicated(opCtx.get(), nss));
}

// Regression test for SERVER-124967: collMod must not allow cappedSize/cappedMax on a view.
TEST_F(CollModTest, CollModCappedSizeOnViewReturnsInvalidOptions) {
    auto opCtx = makeOpCtx();
    NamespaceString backingNss = NamespaceString::createNamespaceString_forTest("test.backingColl");
    NamespaceString viewNss = NamespaceString::createNamespaceString_forTest("test.myView");

    // Create a backing collection and a view on top of it.
    uassertStatusOK(createCollection(opCtx.get(), CreateCommand(backingNss)));
    CollectionOptions viewOptions;
    viewOptions.viewOn = std::string{backingNss.coll()};
    uassertStatusOK(createCollection(opCtx.get(), viewNss, viewOptions, boost::none));

    BSONObjBuilder result;

    // collMod with cappedSize on a view must fail gracefully.
    CollMod cappedSizeCmd(viewNss);
    cappedSizeCmd.setCappedSize(1024LL);
    ASSERT_EQ(ErrorCodes::InvalidOptions,
              processCollModCommand(opCtx.get(), viewNss, cappedSizeCmd, nullptr, &result).code());

    // Same for cappedMax.
    CollMod cappedMaxCmd(viewNss);
    cappedMaxCmd.setCappedMax(100LL);
    ASSERT_EQ(ErrorCodes::InvalidOptions,
              processCollModCommand(opCtx.get(), viewNss, cappedMaxCmd, nullptr, &result).code());
}

}  // namespace
}  // namespace mongo
