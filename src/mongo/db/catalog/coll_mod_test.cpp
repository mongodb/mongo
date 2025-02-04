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

#include "mongo/db/catalog/coll_mod.h"

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/catalog/backwards_compatible_collection_options_util.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/storage_engine_collection_options_flags_parser.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/coll_mod_gen.h"
#include "mongo/db/commands/create_gen.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/timeseries_collmod.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {
TEST(CollModOptionTest, isConvertingIndexToUnique) {
    IDLParserContext ctx("collMod");
    auto requestObj = fromjson("{index: {keyPattern: {a: 1}, unique: true}}");
    auto request = CollModRequest::parse(ctx, requestObj);
    ASSERT_TRUE(isCollModIndexUniqueConversion(request));

    requestObj = fromjson("{index: {keyPattern: {a: 1}, unique: true, hidden: true}}");
    request = CollModRequest::parse(ctx, requestObj);
    ASSERT_TRUE(isCollModIndexUniqueConversion(request));

    requestObj = fromjson(
        "{index: {keyPattern: {a: 1}, unique: true, hidden: true}, validationAction: 'warn'}");
    request = CollModRequest::parse(ctx, requestObj);
    ASSERT_TRUE(isCollModIndexUniqueConversion(request));

    requestObj = fromjson("{index: {keyPattern: {a: 1}, unique: true}, dryRun: true}");
    request = CollModRequest::parse(ctx, requestObj);
    ASSERT_FALSE(isCollModIndexUniqueConversion(request));

    requestObj = fromjson("{index: {keyPattern: {a: 1}, unique: true}, dryRun: false}");
    request = CollModRequest::parse(ctx, requestObj);
    ASSERT_TRUE(isCollModIndexUniqueConversion(request));

    requestObj = fromjson("{index: {keyPattern: {a: 1}, prepareUnique: true}}");
    request = CollModRequest::parse(ctx, requestObj);
    ASSERT_FALSE(isCollModIndexUniqueConversion(request));

    requestObj = fromjson("{validationAction: 'warn'}");
    request = CollModRequest::parse(ctx, requestObj);
    ASSERT_FALSE(isCollModIndexUniqueConversion(request));
}

TEST(CollModOptionTest, makeDryRunRequest) {
    IDLParserContext ctx("collMod");
    auto requestObj = fromjson("{index: {keyPattern: {a: 1}, unique: true}}");
    auto request = CollModRequest::parse(ctx, requestObj);
    auto dryRunRequest = makeCollModDryRunRequest(request);
    ASSERT_TRUE(dryRunRequest.getIndex()->getKeyPattern()->binaryEqual(fromjson("{a: 1}")));
    ASSERT_TRUE(dryRunRequest.getIndex()->getUnique() && *dryRunRequest.getIndex()->getUnique());
    ASSERT_TRUE(dryRunRequest.getDryRun() && *dryRunRequest.getDryRun());

    requestObj = fromjson("{index: {keyPattern: {a: 1}, unique: true, hidden: true}}");
    request = CollModRequest::parse(ctx, requestObj);
    dryRunRequest = makeCollModDryRunRequest(request);
    ASSERT_TRUE(dryRunRequest.getIndex()->getKeyPattern()->binaryEqual(fromjson("{a: 1}")));
    ASSERT_TRUE(dryRunRequest.getIndex()->getUnique() && *dryRunRequest.getIndex()->getUnique());
    ASSERT_FALSE(dryRunRequest.getIndex()->getHidden());
    ASSERT_TRUE(dryRunRequest.getDryRun() && *dryRunRequest.getDryRun());

    requestObj = fromjson(
        "{index: {keyPattern: {a: 1}, unique: true, hidden: true}, validationAction: 'warn'}");
    request = CollModRequest::parse(ctx, requestObj);
    dryRunRequest = makeCollModDryRunRequest(request);
    ASSERT_TRUE(dryRunRequest.getIndex()->getKeyPattern()->binaryEqual(fromjson("{a: 1}")));
    ASSERT_TRUE(dryRunRequest.getIndex()->getUnique() && *dryRunRequest.getIndex()->getUnique());
    ASSERT_FALSE(dryRunRequest.getIndex()->getHidden());
    ASSERT_FALSE(dryRunRequest.getValidationAction());
    ASSERT_TRUE(dryRunRequest.getDryRun() && *dryRunRequest.getDryRun());

    requestObj = fromjson("{index: {keyPattern: {a: 1}, unique: true}, dryRun: false}");
    request = CollModRequest::parse(ctx, requestObj);
    dryRunRequest = makeCollModDryRunRequest(request);
    ASSERT_TRUE(dryRunRequest.getIndex()->getKeyPattern()->binaryEqual(fromjson("{a: 1}")));
    ASSERT_TRUE(dryRunRequest.getIndex()->getUnique() && *dryRunRequest.getIndex()->getUnique());
    ASSERT_TRUE(dryRunRequest.getDryRun() && *dryRunRequest.getDryRun());
}

class CollModTest : public ServiceContextMongoDTest {
protected:
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
        AutoGetCollectionForRead bucketsCollForRead(opCtx.get(), bucketsColl);
        ASSERT_TRUE(bucketsCollForRead->timeseriesBucketingParametersHaveChanged());
        ASSERT_FALSE(*bucketsCollForRead->timeseriesBucketingParametersHaveChanged());
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
        AutoGetCollectionForRead bucketsCollForRead(opCtx.get(), bucketsColl);
        ASSERT_TRUE(bucketsCollForRead->timeseriesBucketingParametersHaveChanged());
        ASSERT_TRUE(*bucketsCollForRead->timeseriesBucketingParametersHaveChanged());
    }

    // Test that both backwards compatibles option and legacy parameter have been properly set
    auto coll =
        CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespace(opCtx.get(), bucketsColl);
    auto catalogEntry =
        DurableCatalog::get(opCtx.get())->getParsedCatalogEntry(opCtx.get(), coll->getCatalogId());
    auto metadata = catalogEntry->metadata;
    boost::optional<bool> optBackwardsCompatibleFlag = getFlagFromStorageEngineBson(
        metadata->options.storageEngine,
        backwards_compatible_collection_options::kTimeseriesBucketingParametersHaveChanged);
    ASSERT_TRUE(optBackwardsCompatibleFlag);
    ASSERT_TRUE(*optBackwardsCompatibleFlag);
    ASSERT_TRUE(metadata->timeseriesBucketingParametersHaveChanged);
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

    AutoGetCollectionForRead bucketsCollForRead(opCtx.get(), bucketsColl);
    ASSERT_FALSE(bucketsCollForRead->timeseriesBucketingParametersHaveChanged());
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
        AutoGetCollectionForRead bucketsCollForRead(opCtx.get(), bucketsColl);
        ASSERT_TRUE(bucketsCollForRead->getTimeseriesBucketsMayHaveMixedSchemaData());
        ASSERT_TRUE(*bucketsCollForRead->getTimeseriesBucketsMayHaveMixedSchemaData());
    }

    // Test that both backwards compatibles option and legacy parameter have been properly set
    auto coll =
        CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespace(opCtx.get(), bucketsColl);
    auto catalogEntry =
        DurableCatalog::get(opCtx.get())->getParsedCatalogEntry(opCtx.get(), coll->getCatalogId());
    auto metadata = catalogEntry->metadata;
    boost::optional<bool> optBackwardsCompatibleFlag = getFlagFromStorageEngineBson(
        metadata->options.storageEngine,
        backwards_compatible_collection_options::kTimeseriesBucketsMayHaveMixedSchemaData);
    ASSERT_TRUE(optBackwardsCompatibleFlag);
    ASSERT_TRUE(*optBackwardsCompatibleFlag);
    ASSERT_TRUE(metadata->timeseriesBucketsMayHaveMixedSchemaData);
    ASSERT_TRUE(*metadata->timeseriesBucketsMayHaveMixedSchemaData);
}

}  // namespace
}  // namespace mongo
