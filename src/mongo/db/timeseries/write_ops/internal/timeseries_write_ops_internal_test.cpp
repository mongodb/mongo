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

#include "mongo/bson/json.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/timeseries/bucket_catalog/global_bucket_catalog.h"
#include "mongo/db/timeseries/write_ops/internal/timeseries_write_ops_internal.h"
#include "mongo/db/timeseries/write_ops/timeseries_write_ops.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo {
namespace {
class TimeseriesWriteOpsInternalTest : public CatalogTestFixture {
public:
    explicit TimeseriesWriteOpsInternalTest(Options options = {})
        : CatalogTestFixture(options.useReplSettings(true)) {}

protected:
    void setUp() override {
        CatalogTestFixture::setUp();
    }
};

TEST_F(TimeseriesWriteOpsInternalTest, BuildBatchedInsertContextsOneBatchNoMetafield) {
    auto opCtx = operationContext();
    NamespaceString ns = NamespaceString::createNamespaceString_forTest(
        "db_timeseries_write_ops_internal_test", "ts");

    ASSERT_OK(createCollection(opCtx,
                               ns.dbName(),
                               BSON("create" << ns.coll() << "timeseries"
                                             << BSON("timeField"
                                                     << "time"))));
    AutoGetCollection bucketsColl(opCtx, ns.makeTimeseriesBucketsNamespace(), LockMode::MODE_IX);

    std::vector<BSONObj> userMeasurementsBatch{
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:05:00.000Z"}, "x":1})"),
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:04:00.000Z"}, "x":2})"),
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:02:00.000Z"}, "x":3})"),
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:03:00.000Z"}, "x":4})"),
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:01:00.000Z"}, "x":5})"),
    };
    auto& bucketCatalog =
        timeseries::bucket_catalog::GlobalBucketCatalog::get(opCtx->getServiceContext());
    tracking::Context trackingContext;
    timeseries::bucket_catalog::ExecutionStatsController stats;
    auto swInsertBatchContextVector =
        timeseries::write_ops::internal::buildBatchedInsertContextsNoMetaField(
            bucketCatalog,
            bucketsColl->uuid(),
            bucketsColl->getTimeseriesOptions().get(),
            userMeasurementsBatch,
            stats,
            trackingContext);

    ASSERT_OK(swInsertBatchContextVector);

    std::vector<size_t> correctIndexOrder{4, 2, 3, 1, 0};
    auto insertBatchContextVector = swInsertBatchContextVector.getValue();
    // Since there was no metadata, all of the measurements should have fit into one batch.
    ASSERT_EQ(insertBatchContextVector.size(), 1);

    auto& insertBatchContext = insertBatchContextVector[0];
    ASSERT_EQ(insertBatchContext.key.metadata.getMetaField(), boost::none);

    for (size_t i = 0; i < insertBatchContext.measurementsTimesAndIndices.size(); i++) {
        auto tuple = insertBatchContext.measurementsTimesAndIndices[i];
        ASSERT_EQ(correctIndexOrder[i], std::get<size_t>(tuple));
        ASSERT_EQ(userMeasurementsBatch[correctIndexOrder[i]].woCompare(std::get<BSONObj>(tuple)),
                  0);
    }
}

TEST_F(TimeseriesWriteOpsInternalTest, BuildBatchedInsertContextsOneBatchWithMetafield) {
    auto opCtx = operationContext();
    NamespaceString ns = NamespaceString::createNamespaceString_forTest(
        "db_timeseries_write_ops_internal_test", "ts");

    ASSERT_OK(createCollection(opCtx,
                               ns.dbName(),
                               BSON("create" << ns.coll() << "timeseries"
                                             << BSON("timeField"
                                                     << "time"
                                                     << "metaField"
                                                     << "meta"))));
    AutoGetCollection bucketsColl(opCtx, ns.makeTimeseriesBucketsNamespace(), LockMode::MODE_IX);

    std::vector<BSONObj> userMeasurementsBatch{
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:05:00.000Z"}, "meta":"a", "x":1})"),
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:04:00.000Z"}, "meta":"a", "x":2})"),
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:02:00.000Z"}, "meta":"a", "x":3})"),
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:03:00.000Z"}, "meta":"a", "x":4})"),
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:01:00.000Z"}, "meta":"a", "x":5})"),
    };

    auto& bucketCatalog =
        timeseries::bucket_catalog::GlobalBucketCatalog::get(opCtx->getServiceContext());
    tracking::Context trackingContext;
    timeseries::bucket_catalog::ExecutionStatsController stats;

    auto swInsertBatchContextVector =
        timeseries::write_ops::internal::buildBatchedInsertContextsWithMetaField(
            bucketCatalog,
            bucketsColl->uuid(),
            bucketsColl->getTimeseriesOptions().get(),
            userMeasurementsBatch,
            "meta",
            stats,
            trackingContext);

    ASSERT_OK(swInsertBatchContextVector);

    std::vector<size_t> correctIndexOrder{4, 2, 3, 1, 0};
    auto insertBatchContextVector = swInsertBatchContextVector.getValue();
    // All of the measurements should have fit into one batch with the same metadata.
    ASSERT_EQ(insertBatchContextVector.size(), 1);

    auto& insertBatchContext = insertBatchContextVector[0];
    ASSERT_EQ(insertBatchContext.key.metadata.getMetaField().get(), "meta");

    for (size_t i = 0; i < insertBatchContext.measurementsTimesAndIndices.size(); i++) {
        auto tuple = insertBatchContext.measurementsTimesAndIndices[i];
        ASSERT_EQ(correctIndexOrder[i], std::get<size_t>(tuple));
        ASSERT_EQ(userMeasurementsBatch[correctIndexOrder[i]].woCompare(std::get<BSONObj>(tuple)),
                  0);
    }
}

TEST_F(TimeseriesWriteOpsInternalTest, BuildBatchedInsertContextsMultipleBatchesWithMetafield) {
    auto opCtx = operationContext();
    NamespaceString ns = NamespaceString::createNamespaceString_forTest(
        "db_timeseries_write_ops_internal_test", "ts");

    ASSERT_OK(createCollection(opCtx,
                               ns.dbName(),
                               BSON("create" << ns.coll() << "timeseries"
                                             << BSON("timeField"
                                                     << "time"
                                                     << "metaField"
                                                     << "meta"))));
    AutoGetCollection bucketsColl(opCtx, ns.makeTimeseriesBucketsNamespace(), LockMode::MODE_IX);

    std::vector<BSONObj> userMeasurementsBatch{
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:01:00.000Z"}, "meta":"b", "x":1})"),
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:04:00.000Z"}, "meta":"c", "x":2})"),
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:05:00.000Z"}, "meta":"a", "x":3})"),
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:07:00.000Z"}, "meta":"a", "x":4})"),
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:03:00.000Z"}, "meta":"b", "x":5})"),
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:02:00.000Z"}, "meta":"c", "x":7})"),
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:06:00.000Z"}, "meta":"a", "x":6})"),
    };

    auto& bucketCatalog =
        timeseries::bucket_catalog::GlobalBucketCatalog::get(opCtx->getServiceContext());
    tracking::Context trackingContext;
    timeseries::bucket_catalog::ExecutionStatsController stats;

    auto swInsertBatchContextVector =
        timeseries::write_ops::internal::buildBatchedInsertContextsWithMetaField(
            bucketCatalog,
            bucketsColl->uuid(),
            bucketsColl->getTimeseriesOptions().get(),
            userMeasurementsBatch,
            "meta",
            stats,
            trackingContext);
    ASSERT_OK(swInsertBatchContextVector);

    stdx::unordered_map<std::string, std::vector<size_t>> metaFieldValueToCorrectIndexOrderMap;
    metaFieldValueToCorrectIndexOrderMap.try_emplace("a", std::initializer_list<size_t>{2, 6, 3});
    metaFieldValueToCorrectIndexOrderMap.try_emplace("b", std::initializer_list<size_t>{0, 4});
    metaFieldValueToCorrectIndexOrderMap.try_emplace("c", std::initializer_list<size_t>{5, 1});

    auto insertBatchContextVector = swInsertBatchContextVector.getValue();
    // There should be three insertBatchContexts for the three distinct meta values.
    ASSERT_EQ(insertBatchContextVector.size(), 3);

    for (size_t i = 0; i < insertBatchContextVector.size(); i++) {
        auto insertBatchContext = insertBatchContextVector[i];
        ASSERT_EQ(insertBatchContext.key.metadata.getMetaField().get(), "meta");
        auto metaFieldValue = insertBatchContext.key.metadata.element().String();
        ASSERT_EQ(insertBatchContext.measurementsTimesAndIndices.size(),
                  metaFieldValueToCorrectIndexOrderMap[metaFieldValue].size());

        for (size_t j = 0; j < insertBatchContext.measurementsTimesAndIndices.size(); j++) {
            auto tuple = insertBatchContext.measurementsTimesAndIndices[j];
            auto measurement = std::get<BSONObj>(tuple);
            ASSERT_EQ(measurement["meta"].String(), metaFieldValue);
            auto index = std::get<size_t>(tuple);
            ASSERT_EQ(index, metaFieldValueToCorrectIndexOrderMap[metaFieldValue][j]);
            ASSERT_EQ(userMeasurementsBatch[index].woCompare(measurement), 0);
        }
    }
}

TEST_F(TimeseriesWriteOpsInternalTest, BuildBatchedInsertContextsMalformedMeasurements) {
    auto opCtx = operationContext();
    NamespaceString ns = NamespaceString::createNamespaceString_forTest(
        "db_timeseries_write_ops_internal_test", "ts");

    ASSERT_OK(createCollection(opCtx,
                               ns.dbName(),
                               BSON("create" << ns.coll() << "timeseries"
                                             << BSON("timeField"
                                                     << "time"
                                                     << "metaField"
                                                     << "meta"))));
    AutoGetCollection bucketsColl(opCtx, ns.makeTimeseriesBucketsNamespace(), LockMode::MODE_IX);

    std::vector<BSONObj> userMeasurementsBatchWithoutMetadata{
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:05:00.000Z"}, "x":1})"),
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:04:00.000Z"}, "x":2})"),
        mongo::fromjson(R"({"x":3})"),  // Malformed measurement, missing time field
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:03:00.000Z"}, "x":4})"),
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:01:00.000Z"}, "x":5})"),
    };
    std::vector<BSONObj> userMeasurementsBatchWithMetadata{
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:05:00.000Z"}, "meta":"a", "x":1})"),
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:04:00.000Z"}, "meta":"a", "x":2})"),
        mongo::fromjson(R"({"x":3,"meta":"a"})"),  // Malformed measurement, missing time field
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:03:00.000Z"}, "meta":"a", "x":4})"),
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:01:00.000Z"}, "meta":"a", "x":5})"),
    };

    auto& bucketCatalog =
        timeseries::bucket_catalog::GlobalBucketCatalog::get(opCtx->getServiceContext());
    auto swInsertBatchContextVector = timeseries::write_ops::internal::buildBatchedInsertContexts(
        bucketCatalog,
        bucketsColl->uuid(),
        bucketsColl->getTimeseriesOptions().get(),
        userMeasurementsBatchWithMetadata);
    ASSERT_EQ(swInsertBatchContextVector.getStatus().code(), ErrorCodes::BadValue);

    // Test the inner helper functions directly as well.
    tracking::Context trackingContext;
    timeseries::bucket_catalog::ExecutionStatsController stats;

    swInsertBatchContextVector =
        timeseries::write_ops::internal::buildBatchedInsertContextsNoMetaField(
            bucketCatalog,
            bucketsColl->uuid(),
            bucketsColl->getTimeseriesOptions().get(),
            userMeasurementsBatchWithoutMetadata,
            stats,
            trackingContext);
    ASSERT_EQ(swInsertBatchContextVector.getStatus().code(), ErrorCodes::BadValue);

    swInsertBatchContextVector =
        timeseries::write_ops::internal::buildBatchedInsertContextsWithMetaField(
            bucketCatalog,
            bucketsColl->uuid(),
            bucketsColl->getTimeseriesOptions().get(),
            userMeasurementsBatchWithMetadata,
            "meta",
            stats,
            trackingContext);
    ASSERT_EQ(swInsertBatchContextVector.getStatus().code(), ErrorCodes::BadValue);
}

}  // namespace
}  // namespace mongo
