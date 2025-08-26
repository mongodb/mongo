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

#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_helpers.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/json.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/create_collection.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/metadata.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/timeseries/timeseries_test_fixture.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <cstddef>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::timeseries::bucket_catalog {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.ts");

class BucketCatalogHelpersTest : public TimeseriesTestFixture {
protected:
    void _insertIntoBucketColl(const NamespaceString& ns, const BSONObj& bucketDoc);
    BSONObj _findSuitableBucket(const NamespaceString& bucketNss,
                                const TimeseriesOptions& options,
                                const BSONObj& measurementDoc);
};

void BucketCatalogHelpersTest::_insertIntoBucketColl(const NamespaceString& ns,
                                                     const BSONObj& bucketDoc) {
    AutoGetCollection autoColl(_opCtx, ns.makeTimeseriesBucketsNamespace(), MODE_IX);
    const CollectionPtr& coll = autoColl.getCollection();
    OpDebug* const nullOpDebug = nullptr;

    {
        WriteUnitOfWork wuow(_opCtx);
        ASSERT_OK(collection_internal::insertDocument(
            _opCtx, coll, InsertStatement(bucketDoc), nullOpDebug));
        wuow.commit();
    }
}

BSONObj BucketCatalogHelpersTest::_findSuitableBucket(const NamespaceString& bucketNss,
                                                      const TimeseriesOptions& options,
                                                      const BSONObj& measurementDoc) {
    uassert(ErrorCodes::InvalidOptions,
            "Missing bucketMaxSpanSeconds option.",
            options.getBucketMaxSpanSeconds());

    Date_t time;
    BSONElement metadata;
    if (!options.getMetaField().has_value()) {
        auto swTime = extractTime(measurementDoc, options.getTimeField());
        ASSERT_OK(swTime);
        time = swTime.getValue();
    } else {
        auto swDocTimeAndMeta = extractTimeAndMeta(
            measurementDoc, options.getTimeField(), options.getMetaField().value());
        ASSERT_OK(swDocTimeAndMeta);
        time = swDocTimeAndMeta.getValue().first;
        metadata = swDocTimeAndMeta.getValue().second;
    }

    boost::optional<BSONObj> normalizedMetadata;
    if (metadata.ok()) {
        allocator_aware::BSONObjBuilder builder;
        metadata::normalize(metadata, builder, kBucketMetaFieldName);
        builder.doneFast();
        normalizedMetadata = BSONObj{builder.bb().release()};
    }

    auto controlMinTimePath = std::string{kControlMinFieldNamePrefix} + options.getTimeField();
    auto maxDataTimeFieldPath = std::string{kDataFieldNamePrefix} + options.getTimeField() + "." +
        std::to_string(gTimeseriesBucketMaxCount - 1);

    // Generate an aggregation request to find a suitable bucket to reopen.
    auto aggregationPipeline = generateReopeningPipeline(
        time,
        normalizedMetadata ? normalizedMetadata->firstElement() : metadata,
        controlMinTimePath,
        maxDataTimeFieldPath,
        *options.getBucketMaxSpanSeconds(),
        /* numberOfActiveBuckets */ gTimeseriesBucketMaxCount);
    AggregateCommandRequest aggRequest(bucketNss, aggregationPipeline);

    // Run an aggregation to find a suitable bucket to reopen.
    DBDirectClient client(_opCtx);
    auto cursor = uassertStatusOK(DBClientCursor::fromAggregationRequest(
        &client, aggRequest, false /* secondaryOk */, false /* useExhaust*/));
    if (cursor->more()) {
        return cursor->next();
    }

    return BSONObj();
}

TEST_F(BucketCatalogHelpersTest, GenerateMinMaxBadBucketDocumentsTest) {
    AutoGetCollection autoColl(_opCtx, _nsNoMeta.makeTimeseriesBucketsNamespace(), MODE_IS);
    const CollatorInterface* collator = autoColl->getDefaultCollator();

    std::vector<BSONObj> docs = {::mongo::fromjson(R"({})"),
                                 ::mongo::fromjson(R"({control: "abc"})"),
                                 ::mongo::fromjson(R"({control: {min: "abc", max: {a: 1}}})"),
                                 ::mongo::fromjson(R"({control: {min: {a: 1}, max: "abc"}})"),
                                 ::mongo::fromjson(R"({control: {min: {a : 1}}})"),
                                 ::mongo::fromjson(R"({control: {max: {a: 1}}})"),
                                 ::mongo::fromjson(R"({control: {min: {}, max: {a: 1}}})"),
                                 ::mongo::fromjson(R"({control: {min: {a: 1}, max: {}}})")};

    for (const BSONObj& doc : docs) {
        tracking::Context trackingContext;
        StatusWith<MinMax> swMinMax = generateMinMaxFromBucketDoc(trackingContext, doc, collator);
        ASSERT_NOT_OK(swMinMax.getStatus());
    }
}

TEST_F(BucketCatalogHelpersTest, GenerateMinMaxTest) {
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IS);
    const CollatorInterface* collator = autoColl->getDefaultCollator();

    std::vector<BSONObj> docs = {
        ::mongo::fromjson(R"({control:{min: {a: 1}, max: {a: 2}}})"),
        ::mongo::fromjson(R"({control: {min: {a: [1, 2, 3]}, max: {a: [4, 5, 6]}}})"),
        ::mongo::fromjson(R"({control: {min: {a: {b: 1}}, max: {a: {b: 2}}}})"),
        ::mongo::fromjson(R"({control: {min: {a: {b: 1, c: [1]}}, max: {a: {b: 2, c: [2]}}}})"),
        ::mongo::fromjson(R"({control: {min: {a: "A", b: 5}, max: {a: "Z", b: 500}}})"),
        ::mongo::fromjson(R"({control: {min: {_id: ObjectId("628d1d8e14a44c3a3fdf9522")},
                                        max: {_id: ObjectId("628d1d9014a44c3a3fdf9524")}}})"),
        ::mongo::fromjson(R"({control: {min:{a: 1, b: {c: 1, d: [1, 2, 3]}, e: [1, 2, 3]},
                                        max:{a: 2, b: {c: 2, d: [4, 5, 6]}, e: [4, 5, 6]}}})")};

    for (const BSONObj& doc : docs) {
        tracking::Context trackingContext;
        StatusWith<MinMax> swMinMax = generateMinMaxFromBucketDoc(trackingContext, doc, collator);
        ASSERT_OK(swMinMax.getStatus());

        MinMax minmax = std::move(swMinMax.getValue());

        ASSERT_BSONOBJ_BINARY_EQ(doc.getObjectField("control").getObjectField("min"), minmax.min());
        ASSERT_BSONOBJ_BINARY_EQ(doc.getObjectField("control").getObjectField("max"), minmax.max());
    }
}

TEST_F(BucketCatalogHelpersTest, GenerateMinMaxWithLowerCaseFirstCollationTest) {
    ASSERT_OK(createCollection(_opCtx,
                               kNss.dbName(),
                               BSON("create" << kNss.coll() << "timeseries"
                                             << BSON("timeField" << _timeField) << "collation"
                                             << BSON("locale" << "en_US"
                                                              << "caseFirst"
                                                              << "lower"))));

    AutoGetCollection autoColl(_opCtx, kNss.makeTimeseriesBucketsNamespace(), MODE_IS);
    const CollatorInterface* collator = autoColl->getDefaultCollator();

    // Lowercase compares less than uppercase with a {caseFirst: "lower"} collator.
    BSONObj doc = ::mongo::fromjson(R"({control: {min: {field: "a"}, max: {field: "A"}}})");

    tracking::Context trackingContext;
    StatusWith<MinMax> swMinMax = generateMinMaxFromBucketDoc(trackingContext, doc, collator);
    ASSERT_OK(swMinMax.getStatus());

    MinMax minmax = std::move(swMinMax.getValue());

    ASSERT_BSONOBJ_BINARY_EQ(doc.getObjectField("control").getObjectField("min"), minmax.min());
    ASSERT_BSONOBJ_BINARY_EQ(doc.getObjectField("control").getObjectField("max"), minmax.max());
    _addNsToValidate(kNss);
}

TEST_F(BucketCatalogHelpersTest, GenerateMinMaxWithUpperCaseFirstCollationTest) {
    ASSERT_OK(createCollection(_opCtx,
                               kNss.dbName(),
                               BSON("create" << kNss.coll() << "timeseries"
                                             << BSON("timeField" << _timeField) << "collation"
                                             << BSON("locale" << "en_US"
                                                              << "caseFirst"
                                                              << "upper"))));

    AutoGetCollection autoColl(_opCtx, kNss.makeTimeseriesBucketsNamespace(), MODE_IS);
    const CollatorInterface* collator = autoColl->getDefaultCollator();

    // Uppercase compares less than lowercase with a {caseFirst: "upper"} collator.
    BSONObj doc = ::mongo::fromjson(R"({control: {min: {field: "A"}, max: {field: "a"}}})");

    tracking::Context trackingContext;
    StatusWith<MinMax> swMinMax = generateMinMaxFromBucketDoc(trackingContext, doc, collator);
    ASSERT_OK(swMinMax.getStatus());

    MinMax minmax = std::move(swMinMax.getValue());

    ASSERT_BSONOBJ_BINARY_EQ(doc.getObjectField("control").getObjectField("min"), minmax.min());
    ASSERT_BSONOBJ_BINARY_EQ(doc.getObjectField("control").getObjectField("max"), minmax.max());
    _addNsToValidate(kNss);
}

TEST_F(BucketCatalogHelpersTest, GenerateMinMaxSucceedsWithMixedSchemaBucketDocumentTest) {
    AutoGetCollection autoColl(_opCtx, _nsNoMeta.makeTimeseriesBucketsNamespace(), MODE_IS);
    const CollatorInterface* collator = autoColl->getDefaultCollator();

    std::vector<BSONObj> docs = {::mongo::fromjson(R"({control:{min: {a: 1}, max: {a: {}}}})"),
                                 ::mongo::fromjson(R"({control:{min: {a: {}}, max: {a: 1}}})"),
                                 ::mongo::fromjson(R"({control:{min: {a: []}, max: {a: {}}}})"),
                                 ::mongo::fromjson(R"({control:{min: {a: 1}, max: {a: "foo"}}})")};

    for (const BSONObj& doc : docs) {
        tracking::Context trackingContext;
        StatusWith<MinMax> swMinMax = generateMinMaxFromBucketDoc(trackingContext, doc, collator);
        ASSERT_OK(swMinMax.getStatus());
    }
}

TEST_F(BucketCatalogHelpersTest, GenerateSchemaFailsWithMixedSchemaBucketDocumentTest) {
    AutoGetCollection autoColl(_opCtx, _nsNoMeta.makeTimeseriesBucketsNamespace(), MODE_IS);
    const CollatorInterface* collator = autoColl->getDefaultCollator();

    std::vector<BSONObj> docs = {::mongo::fromjson(R"({control:{min: {a: 1}, max: {a: {}}}})"),
                                 ::mongo::fromjson(R"({control:{min: {a: {}}, max: {a: 1}}})"),
                                 ::mongo::fromjson(R"({control:{min: {a: []}, max: {a: {}}}})"),
                                 ::mongo::fromjson(R"({control:{min: {a: 1}, max: {a: "foo"}}})")};

    for (const BSONObj& doc : docs) {
        tracking::Context trackingContext;
        StatusWith<Schema> swSchema = generateSchemaFromBucketDoc(trackingContext, doc, collator);
        ASSERT_NOT_OK(swSchema.getStatus());
    }
}

TEST_F(BucketCatalogHelpersTest, GenerateSchemaWithInvalidMeasurementsTest) {
    AutoGetCollection autoColl(_opCtx, _nsNoMeta.makeTimeseriesBucketsNamespace(), MODE_IS);
    const CollatorInterface* collator = autoColl->getDefaultCollator();

    // First item: Bucket document to generate the schema representation of.
    // Second item: measurement that is incompatible with the generated schema.
    std::vector<std::pair<BSONObj, BSONObj>> docs = {
        {::mongo::fromjson(R"({control:{min: {a: 1}, max: {a: 2}}})"),
         ::mongo::fromjson(R"({a: {}})")},
        {::mongo::fromjson(R"({control:{min: {a: 1}, max: {a: 2}}})"),
         ::mongo::fromjson(R"({a: []})")},
        {::mongo::fromjson(R"({control:{min: {a: 1}, max: {a: 2}}})"),
         ::mongo::fromjson(R"({a: "1"})")},
        {::mongo::fromjson(R"({control:{min: {a: "a"}, max: {a: "aa"}}})"),
         ::mongo::fromjson(R"({a: 123})")},
        {::mongo::fromjson(R"({control:{min: {a: [1, 2, 3]}, max: {a: [4, 5, 6]}}})"),
         ::mongo::fromjson(R"({a: {}})")},
        {::mongo::fromjson(R"({control:{min: {a: [1, 2, 3]}, max: {a: [4, 5, 6]}}})"),
         ::mongo::fromjson(R"({a: 123})")},
        {::mongo::fromjson(R"({control:{min: {a: [1, 2, 3]}, max: {a: [4, 5, 6]}}})"),
         ::mongo::fromjson(R"({a: "abc"})")},
        {::mongo::fromjson(R"({control:{min: {a: {b: 1}}, max: {a: {b: 2}}}})"),
         ::mongo::fromjson(R"({a: []})")},
        {::mongo::fromjson(R"({control:{min: {a: {b: 1}}, max: {a: {b: 2}}}})"),
         ::mongo::fromjson(R"({a: {b: "abc"}})")},
        {::mongo::fromjson(R"({control:{min: {a: {b: 1}}, max: {a: {b: 2}}}})"),
         ::mongo::fromjson(R"({a: {b: []}})")}};

    for (const auto& [minMaxDoc, measurementDoc] : docs) {
        tracking::Context trackingContext;
        StatusWith<Schema> swSchema =
            generateSchemaFromBucketDoc(trackingContext, minMaxDoc, collator);
        ASSERT_OK(swSchema.getStatus());

        Schema schema = std::move(swSchema.getValue());

        auto result = schema.update(measurementDoc, /*metaField=*/boost::none, collator);
        ASSERT(result == Schema::UpdateStatus::Failed);
    }
}

TEST_F(BucketCatalogHelpersTest, GenerateSchemaWithValidMeasurementsTest) {
    AutoGetCollection autoColl(_opCtx, _nsNoMeta.makeTimeseriesBucketsNamespace(), MODE_IS);
    const CollatorInterface* collator = autoColl->getDefaultCollator();

    // First item: Bucket document to generate the schema representation of.
    // Second item: measurement that is compatible with the generated schema.
    std::vector<std::pair<BSONObj, BSONObj>> docs = {
        {::mongo::fromjson(R"({control:{min: {a: 1}, max: {a: 2}}})"),
         ::mongo::fromjson(R"({a: 1})")},
        {::mongo::fromjson(R"({control:{min: {a: 1}, max: {a: 2}}})"),
         ::mongo::fromjson(R"({a: 5})")},
        {::mongo::fromjson(R"({control:{min: {a: "a"}, max: {a: "aa"}}})"),
         ::mongo::fromjson(R"({a: "aaa"})")},
        {::mongo::fromjson(R"({control:{min: {a: [1, 2, 3]}, max: {a: [4, 5, 6]}}})"),
         ::mongo::fromjson(R"({a: [7, 8, 9]})")},
        {::mongo::fromjson(R"({control:{min: {a: {b: 1}}, max: {a: {b: 2}}}})"),
         ::mongo::fromjson(R"({a: {b: 3}})")}};

    for (const auto& [minMaxDoc, measurementDoc] : docs) {
        tracking::Context trackingContext;
        StatusWith<Schema> swSchema =
            generateSchemaFromBucketDoc(trackingContext, minMaxDoc, collator);
        ASSERT_OK(swSchema.getStatus());

        Schema schema = std::move(swSchema.getValue());

        auto result = schema.update(measurementDoc, /*metaField=*/boost::none, collator);
        ASSERT(result == Schema::UpdateStatus::Updated);
    }
}

TEST_F(BucketCatalogHelpersTest, FindSuitableBucketForMeasurements) {
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);
    ASSERT(autoColl->getTimeseriesOptions() && autoColl->getTimeseriesOptions()->getMetaField());

    auto tsOptions = *autoColl->getTimeseriesOptions();
    auto metaFieldName = *tsOptions.getMetaField();

    std::vector<BSONObj> bucketDocs = {mongo::fromjson(
                                           R"({
            "_id":{"$oid":"61067eb0de4e031499bc4046"},
            "control":{"version":1,"min":{"_id":1,"time":{"$date":"2021-08-01T11:00:00Z"},"a":1},
                                   "max":{"_id":3,"time":{"$date":"2021-08-01T11:00:00Z"},"a":3},
                       "closed":false},
            "meta":1,
            "data":{"time":{"0":{"$date":"2021-08-01T11:00:00Z"},
                            "1":{"$date":"2021-08-01T11:00:00Z"},
                            "2":{"$date":"2021-08-01T11:00:00Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "_id": {"0":1,"1":2,"2":3}}})"),
                                       mongo::fromjson(
                                           R"(
            {"_id":{"$oid":"61068cc0de4e031499bc4047"},
            "control":{"version":1,"min":{"_id":4,"time":{"$date":"2021-08-01T12:00:00Z"},"a":1},
                                   "max":{"_id":6,"time":{"$date":"2021-08-01T12:00:00Z"},"a":3}},
            "meta":2,
            "data":{"time":{"0":{"$date":"2021-08-01T12:00:00Z"},
                            "1":{"$date":"2021-08-01T12:00:00Z"},
                            "2":{"$date":"2021-08-01T12:00:00Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "_id": {"0":4,"1":5,"2":6}
                    }})"),
                                       mongo::fromjson(
                                           R"({
            "_id":{"$oid":"61069ad0de4e031499bc404b"},
            "control":{"version":1,"min":{"_id":7,"time":{"$date":"2021-08-01T13:00:00Z"},"a":1},
                                   "max":{"_id":9,"time":{"$date":"2021-08-01T13:00:00Z"},"a":3},
                       "closed":false},
            "meta":3,
            "data":{"time":{"0":{"$date":"2021-08-01T13:00:00Z"},
                            "1":{"$date":"2021-08-01T13:00:00Z"},
                            "2":{"$date":"2021-08-01T13:00:00Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "_id": {"0":7,"1":8,"2":9}}})")};

    // Insert bucket documents into the system.buckets collection.
    for (const auto& doc : bucketDocs) {
        _insertIntoBucketColl(_ns1, doc);
    }

    auto time1 = dateFromISOString("2021-08-01T11:30:00Z");
    auto time2 = dateFromISOString("2021-08-01T12:30:00Z");
    auto time3 = dateFromISOString("2021-08-01T13:30:00Z");
    std::vector<BSONObj> docsWithSuitableBuckets = {
        BSON("_id" << 1 << _timeField << time1.getValue() << _metaField << 1),
        BSON("_id" << 2 << _timeField << time2.getValue() << _metaField << 2),
        BSON("_id" << 3 << _timeField << time3.getValue() << _metaField << 3)};

    // Iterate through the measurement documents and verify that we can find a suitable bucket to
    // insert into.
    for (size_t i = 0; i < docsWithSuitableBuckets.size(); ++i) {
        const auto& doc = docsWithSuitableBuckets[i];
        auto result = _findSuitableBucket(_ns1.makeTimeseriesBucketsNamespace(), tsOptions, doc);
        ASSERT_FALSE(result.isEmpty());
        ASSERT_EQ(bucketDocs[i]["_id"].OID(), result["_id"].OID());
    }

    // Verify that documents without a meta field are only eligible to be inserted into buckets with
    // no meta field specified.
    {
        std::vector<BSONObj> docsWithOutMeta;
        for (const auto& doc : docsWithSuitableBuckets) {
            docsWithOutMeta.push_back(doc.removeField(_metaField));
        }

        for (size_t i = 0; i < docsWithOutMeta.size(); ++i) {
            const auto& doc = docsWithOutMeta[i];
            auto result =
                _findSuitableBucket(_ns1.makeTimeseriesBucketsNamespace(), tsOptions, doc);
            ASSERT(result.isEmpty());
        }

        // Verify that a document with no meta field is suitable with a valid bucket also missing
        // the meta field.
        auto metalessDoc = BSON("_id" << 4 << _timeField << time3.getValue());
        auto metalessBucket = mongo::fromjson(
            R"({
            "_id":{"$oid":"61069ad0de4e031499bc404c"},
            "control":{"version":1,"min":{"_id":10,"time":{"$date":"2021-08-01T13:00:00Z"},"a":1},
                                   "max":{"_id":12,"time":{"$date":"2021-08-01T13:00:00Z"},"a":3},
                       "closed":false},
            "data":{"time":{"0":{"$date":"2021-08-01T13:00:00Z"},
                            "1":{"$date":"2021-08-01T13:00:00Z"},
                            "2":{"$date":"2021-08-01T13:00:00Z"}},
                    "a":{"0":1,"1":2,"2":3},
                     "_id": {"0":10,"1":11,"2":12}}})");
        _insertIntoBucketColl(_ns1, metalessBucket);

        auto result =
            _findSuitableBucket(_ns1.makeTimeseriesBucketsNamespace(), tsOptions, metalessDoc);
        ASSERT_FALSE(result.isEmpty());
        ASSERT_EQ(metalessBucket["_id"].OID(), result["_id"].OID());
    }

    std::vector<BSONObj> docsWithoutSuitableBuckets = {
        // Mismatching time field with corresponding bucket meta field.
        BSON("_id" << 1 << _timeField << Date_t::now() << _metaField << 1),
        // Mismatching meta field with corresponding bucket time range.
        BSON("_id" << 2 << _timeField << time2.getValue() << _metaField << 100000)};

    // Without a matching meta field or a time within a buckets time range, we should not find any
    // suitable buckets.
    for (const auto& doc : docsWithoutSuitableBuckets) {
        BSONObj measurementMeta =
            (doc.hasField(metaFieldName)) ? doc.getField(metaFieldName).wrap() : BSONObj();
        auto result = _findSuitableBucket(_ns1.makeTimeseriesBucketsNamespace(), tsOptions, doc);
        ASSERT(result.isEmpty());
    }
}

TEST_F(BucketCatalogHelpersTest, FindDocumentFromOID) {
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);
    ASSERT(autoColl->getTimeseriesOptions() && autoColl->getTimeseriesOptions()->getMetaField());

    std::vector<BSONObj> bucketDocs = {mongo::fromjson(R"({
            "_id":{"$oid":"61067eb0de4e031499bc4046"},
            "control":{"version":1,"min":{"_id":1,"time":{"$date":"2021-08-01T11:00:00Z"},"a":1},
                                   "max":{"_id":3,"time":{"$date":"2021-08-01T11:00:00Z"},"a":3},
                       "closed":false},
            "meta":1,
            "data":{"time":{"0":{"$date":"2021-08-01T11:00:00Z"},
                            "1":{"$date":"2021-08-01T11:00:00Z"},
                            "2":{"$date":"2021-08-01T11:00:00Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "_id":{"0":1,"1":2,"2":3}}})"),
                                       mongo::fromjson(
                                           R"(
            {"_id":{"$oid":"61068cc0de4e031499bc4047"},
            "control":{"version":1,"min":{"_id":4,"time":{"$date":"2021-08-01T12:00:00Z"},"a":1},
                                   "max":{"_id":6,"time":{"$date":"2021-08-01T12:00:00Z"},"a":3}},
            "meta":2,
            "data":{"time":{"0":{"$date":"2021-08-01T12:00:00Z"},
                            "1":{"$date":"2021-08-01T12:00:00Z"},
                            "2":{"$date":"2021-08-01T12:00:00Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "_id":{"0":4,"1":5,"2":6}}})"),
                                       mongo::fromjson(
                                           R"({
            "_id":{"$oid":"61069ad0de4e031499bc4048"},
            "control":{"version":1,"min":{"_id":7,"time":{"$date":"2021-08-01T13:00:00Z"},"a":1},
                                   "max":{"_id":9,"time":{"$date":"2021-08-01T13:00:00Z"},"a":3},
                       "closed":false},
            "meta":3,
            "data":{"time":{"0":{"$date":"2021-08-01T13:00:00Z"},
                            "1":{"$date":"2021-08-01T13:00:00Z"},
                            "2":{"$date":"2021-08-01T13:00:00Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "_id":{"0":7,"1":8,"2":9}}})")};

    // Insert bucket documents into the system.buckets collection.
    for (const auto& doc : bucketDocs) {
        _insertIntoBucketColl(_ns1, doc);
    }

    auto findDocFromOID = [opCtx = _opCtx, coll = (*autoColl).get()](const OID& bucketId) {
        Snapshotted<BSONObj> bucketObj;
        auto rid = record_id_helpers::keyForOID(bucketId);
        return (coll->findDoc(opCtx, rid, &bucketObj)) ? bucketObj.value() : BSONObj();
    };

    // Given a valid OID for a bucket document, we should be able to retrieve the full bucket
    // document.
    for (const auto& doc : bucketDocs) {
        const auto bucketId = doc["_id"].OID();
        auto retrievedBucket = findDocFromOID(bucketId);
        ASSERT(!retrievedBucket.isEmpty());
        ASSERT_BSONOBJ_EQ(retrievedBucket, doc);
    }

    // For non-existent OIDs, we don't expect to retrieve anything.
    std::vector<OID> nonExistentOIDs = {OID("26e7e6ec27c28d338ab29200"),
                                        OID("90e7e6ec27c28d338ab29200"),
                                        OID("00e7e6ec27c28d338ab29200")};
    for (const auto& oid : nonExistentOIDs) {
        auto retrievedBucket = findDocFromOID(oid);
        ASSERT(retrievedBucket.isEmpty());
    }
}

TEST_F(BucketCatalogHelpersTest, FindSuitableCompressedBucketForMeasurement) {
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);
    ASSERT(autoColl->getTimeseriesOptions() && autoColl->getTimeseriesOptions()->getMetaField());
    auto tsOptions = *autoColl->getTimeseriesOptions();
    BSONObj bucketDoc = mongo::fromjson(R"({"_id":{"$oid":"61068cc0de4e031499bc4049"},
        "control":{"version":2,"min":{"time":{"$date":"2021-08-01T12:00:00Z"},"_id":ObjectId('67f6e10b91f966678d89720b'),"a":1},
                               "max":{"time":{"$date":"2021-08-01T12:00:00Z"},"_id":ObjectId('67f6e14691f966678d89720f'),"a":3},
        "count":3},
        "meta":0,"data":{"time":{"$binary":"CQAAzpUBewEAAIANAAAAAAAAAAA=","$type":"07"},
        "_id":{"$binary":"BwBn9uELkflmZ42JcguATQAHAPynAQAA","$type":"07"},
        "a":{"$binary":"AQAAAAAAAADwP5AtAAAACAAAAAA=","$type":"07"}}})");

    // Insert bucket document into the system.buckets collection.
    _insertIntoBucketColl(_ns1, bucketDoc);

    auto time = dateFromISOString("2021-08-01T12:00:00Z");
    BSONObj docWithSuitableBucket =
        BSON("_id" << 1 << _timeField << time.getValue() << _metaField << 0);

    // Verify that we can find a suitable bucket to insert into.
    auto result = _findSuitableBucket(
        _ns1.makeTimeseriesBucketsNamespace(), tsOptions, docWithSuitableBucket);
    ASSERT_FALSE(result.isEmpty());
    ASSERT_EQ(bucketDoc["_id"].OID(), result["_id"].OID());
}

}  // namespace
}  // namespace mongo::timeseries::bucket_catalog
