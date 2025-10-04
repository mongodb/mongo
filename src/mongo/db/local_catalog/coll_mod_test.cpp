/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/local_catalog/coll_mod.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/client.h"
#include "mongo/db/local_catalog/backwards_compatible_collection_options_util.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/create_collection.h"
#include "mongo/db/local_catalog/ddl/coll_mod_gen.h"
#include "mongo/db/local_catalog/ddl/create_gen.h"
#include "mongo/db/local_catalog/durable_catalog.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/mdb_catalog.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/timeseries_collmod.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/vector_clock/vector_clock_mutable.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/idl/server_parameter_test_controller.h"
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
                                     PlacementConcern(boost::none, ShardVersion::UNSHARDED()),
                                     repl::ReadConcernArgs::get(opCtx),
                                     AcquisitionPrerequisites::kRead),
        MODE_IS);
}

TEST_F(CollModTest, CollModTimeseriesWithFixedBucket) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagTSBucketingParametersUnchanged", true);

    NamespaceString curNss = NamespaceString::createNamespaceString_forTest("test.curColl");
    auto bucketsColl =
        NamespaceString::createNamespaceString_forTest("test.system.buckets.curColl");

    auto opCtx = makeOpCtx();
    auto tsOptions = TimeseriesOptions("t");
    tsOptions.setBucketRoundingSeconds(100);
    tsOptions.setBucketMaxSpanSeconds(100);
    CreateCommand cmd = CreateCommand(curNss);
    cmd.getCreateCollectionRequest().setTimeseries(std::move(tsOptions));
    uassertStatusOK(createCollection(opCtx.get(), cmd));

    // Run collMod without changing the bucket span and validate that the
    // timeseriesBucketingParametersHaveChanged() returns false.
    CollMod collModCmd(curNss);
    CollModRequest collModRequest;
    std::variant<std::string, std::int64_t> expireAfterSeconds = 100;
    collModRequest.setExpireAfterSeconds(expireAfterSeconds);
    collModCmd.setCollModRequest(collModRequest);
    BSONObjBuilder result;
    uassertStatusOK(timeseries::processCollModCommandWithTimeSeriesTranslation(
        opCtx.get(), curNss, collModCmd, true, &result));
    {
        const auto bucketsCollForRead = acquireCollForRead(opCtx.get(), bucketsColl);
        // TODO(SERVER-101611): Set *timeseriesBucketingParametersHaveChanged to false on create
        ASSERT_FALSE(
            bucketsCollForRead.getCollectionPtr()->timeseriesBucketingParametersHaveChanged());
    }

    // Run collMod which changes the bucket span and validate that the
    // timeseriesBucketingParametersHaveChanged() returns true.
    CollModTimeseries collModTs;
    collModTs.setBucketMaxSpanSeconds(200);
    collModTs.setBucketRoundingSeconds(200);
    collModRequest.setTimeseries(std::move(collModTs));
    collModCmd.setCollModRequest(std::move(collModRequest));
    uassertStatusOK(timeseries::processCollModCommandWithTimeSeriesTranslation(
        opCtx.get(), curNss, collModCmd, true, &result));
    {
        const auto bucketsCollForRead = acquireCollForRead(opCtx.get(), bucketsColl);
        ASSERT_TRUE(
            bucketsCollForRead.getCollectionPtr()->timeseriesBucketingParametersHaveChanged());
        ASSERT_TRUE(
            *bucketsCollForRead.getCollectionPtr()->timeseriesBucketingParametersHaveChanged());
    }

    // Test that the backwards compatible option has been properly set
    auto coll =
        CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespace(opCtx.get(), bucketsColl);
    auto catalogEntry = durable_catalog::getParsedCatalogEntry(
        opCtx.get(), coll->getCatalogId(), MDBCatalog::get(opCtx.get()));
    auto metadata = catalogEntry->metadata;
    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    boost::optional<bool> optBackwardsCompatibleFlag = storageEngine->getFlagFromStorageOptions(
        metadata->options.storageEngine,
        backwards_compatible_collection_options::kTimeseriesBucketingParametersHaveChanged);
    ASSERT_TRUE(optBackwardsCompatibleFlag);
    ASSERT_TRUE(*optBackwardsCompatibleFlag);
}

TEST_F(CollModTest, TimeseriesBucketingParameterChanged) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagTSBucketingParametersUnchanged", true);

    NamespaceString curNss = NamespaceString::createNamespaceString_forTest("test.curColl");
    auto bucketsColl =
        NamespaceString::createNamespaceString_forTest("test.system.buckets.curColl");

    auto opCtx = makeOpCtx();
    auto tsOptions = TimeseriesOptions("t");
    tsOptions.setBucketRoundingSeconds(100);
    tsOptions.setBucketMaxSpanSeconds(100);
    CreateCommand cmd = CreateCommand(curNss);
    cmd.getCreateCollectionRequest().setTimeseries(std::move(tsOptions));
    uassertStatusOK(createCollection(opCtx.get(), cmd));

    uassertStatusOK(writeConflictRetry(
        opCtx.get(), "unitTestTimeseriesBucketingParameterChanged", bucketsColl, [&] {
            WriteUnitOfWork wunit(opCtx.get());

            AutoGetCollection collection(opCtx.get(), bucketsColl, MODE_X);
            CollectionWriter writer{opCtx.get(), collection};
            auto writableColl = writer.getWritableCollection(opCtx.get());
            writableColl->setTimeseriesBucketingParametersChanged(opCtx.get(), boost::none);

            wunit.commit();
            return Status::OK();
        }));

    const auto bucketsCollForRead = acquireCollForRead(opCtx.get(), bucketsColl);
    ASSERT_FALSE(bucketsCollForRead.getCollectionPtr()->timeseriesBucketingParametersHaveChanged());
}

TEST_F(CollModTest, TimeseriesLegacyBucketingParameterChangedRemoval) {
    NamespaceString curNss = NamespaceString::createNamespaceString_forTest("test.curColl");
    auto bucketsColl =
        NamespaceString::createNamespaceString_forTest("test.system.buckets.curColl");

    auto opCtx = makeOpCtx();
    CreateCommand cmd = CreateCommand(curNss);
    cmd.setTimeseries(TimeseriesOptions("t"));
    uassertStatusOK(createCollection(opCtx.get(), cmd));

    auto catalogId = CollectionCatalog::get(opCtx.get())
                         ->lookupCollectionByNamespace(opCtx.get(), bucketsColl)
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
    CollMod collModCmd(bucketsColl);
    collModCmd.set_removeLegacyTimeseriesBucketingParametersHaveChanged(true);
    BSONObjBuilder result;
    uassertStatusOK(processCollModCommand(opCtx.get(), bucketsColl, collModCmd, nullptr, &result));

    {
        auto catalogEntry =
            durable_catalog::getParsedCatalogEntry(opCtx.get(), catalogId, mdbCatalog);
        ASSERT_FALSE(catalogEntry->metadata->timeseriesBucketingParametersHaveChanged_DO_NOT_USE);
    }
}

TEST_F(CollModTest, CollModTimeseriesMixedSchemaData) {
    NamespaceString curNss = NamespaceString::createNamespaceString_forTest("test.curColl");
    auto bucketsColl =
        NamespaceString::createNamespaceString_forTest("test.system.buckets.curColl");

    auto opCtx = makeOpCtx();
    auto tsOptions = TimeseriesOptions("t");
    CreateCommand cmd = CreateCommand(curNss);
    cmd.getCreateCollectionRequest().setTimeseries(std::move(tsOptions));
    uassertStatusOK(createCollection(opCtx.get(), cmd));

    CollMod collModCmd(curNss);
    CollModRequest collModRequest;
    collModRequest.setTimeseriesBucketsMayHaveMixedSchemaData(true);
    collModCmd.setCollModRequest(collModRequest);
    BSONObjBuilder result;
    uassertStatusOK(timeseries::processCollModCommandWithTimeSeriesTranslation(
        opCtx.get(), curNss, collModCmd, true, &result));
    {
        const auto bucketsCollForRead = acquireCollForRead(opCtx.get(), bucketsColl);
        auto mixedSchemaState =
            bucketsCollForRead.getCollectionPtr()->getTimeseriesMixedSchemaBucketsState();
        ASSERT_TRUE(mixedSchemaState.isValid());
        ASSERT_TRUE(mixedSchemaState.mustConsiderMixedSchemaBucketsInReads());
        ASSERT_TRUE(mixedSchemaState.canStoreMixedSchemaBucketsSafely());
    }

    // Test that both backwards compatibles option and legacy parameter have been properly set
    auto coll =
        CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespace(opCtx.get(), bucketsColl);
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

// Regression test for SERVER-104640. Test that the MixedSchema and BucketingParametersHaveChanged
// timeseries flags can be read at a point-in-time from the collection catalog.
TEST_F(CollModTimestampedTest, CollModTimeseriesMixedSchemaFlagPointInTimeLookup) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagTSBucketingParametersUnchanged", true);

    NamespaceString curNss = NamespaceString::createNamespaceString_forTest("test.curColl");
    auto bucketsColl =
        NamespaceString::createNamespaceString_forTest("test.system.buckets.curColl");

    auto opCtx = makeOpCtx();

    CreateCommand cmd = CreateCommand(curNss);
    cmd.getCreateCollectionRequest().setTimeseries(TimeseriesOptions("t"));
    uassertStatusOK(createCollection(opCtx.get(), cmd));

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
                             ->establishConsistentCollection(opCtx.get(), bucketsColl, collModTime);
        ASSERT_TRUE(collAfter->getTimeseriesMixedSchemaBucketsState()
                        .mustConsiderMixedSchemaBucketsInReads());
        ASSERT_TRUE(
            collAfter->getTimeseriesMixedSchemaBucketsState().canStoreMixedSchemaBucketsSafely());
        ASSERT_EQ(true, collAfter->timeseriesBucketingParametersHaveChanged());
    }

    // Check the collection at a timestamp before the collMod
    {
        ReadSourceScope scope(opCtx.get(), RecoveryUnit::ReadSource::kProvided, collModTime - 1);
        auto collBefore =
            CollectionCatalog::get(opCtx.get())
                ->establishConsistentCollection(opCtx.get(), bucketsColl, collModTime - 1);
        ASSERT_FALSE(collBefore->getTimeseriesMixedSchemaBucketsState()
                         .mustConsiderMixedSchemaBucketsInReads());
        ASSERT_FALSE(
            collBefore->getTimeseriesMixedSchemaBucketsState().canStoreMixedSchemaBucketsSafely());
        ASSERT_NE(true, collBefore->timeseriesBucketingParametersHaveChanged());
    }
}

}  // namespace
}  // namespace mongo
