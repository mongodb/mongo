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

#include "mongo/bson/json.h"
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_helpers.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo::timeseries::bucket_catalog {
namespace {

const NamespaceString kNss = NamespaceString("test.ts");

class BucketCatalogHelpersTest : public CatalogTestFixture {
protected:
    StringData _timeField = "time";
    StringData _metaField = "mm";

    void _insertIntoBucketColl(const BSONObj& bucketDoc);
    BSONObj _findSuitableBucket(OperationContext* opCtx,
                                const NamespaceString& bucketNss,
                                const TimeseriesOptions& options,
                                const BSONObj& measurementDoc);
};

void BucketCatalogHelpersTest::_insertIntoBucketColl(const BSONObj& bucketDoc) {
    AutoGetCollection autoColl(operationContext(), kNss.makeTimeseriesBucketsNamespace(), MODE_IX);
    const CollectionPtr& coll = autoColl.getCollection();
    OpDebug* const nullOpDebug = nullptr;

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(collection_internal::insertDocument(
            operationContext(), coll, InsertStatement(bucketDoc), nullOpDebug));
        wuow.commit();
    }
}

BSONObj BucketCatalogHelpersTest::_findSuitableBucket(OperationContext* opCtx,
                                                      const NamespaceString& bucketNss,
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

    auto controlMinTimePath = kControlMinFieldNamePrefix.toString() + options.getTimeField();

    boost::optional<BSONObj> normalizedMetadata;
    if (metadata.ok()) {
        BSONObjBuilder builder;
        normalizeMetadata(&builder, metadata, kBucketMetaFieldName);
        normalizedMetadata = builder.obj();
    }

    // Generate all the filters we need to add to our 'find' query for a suitable bucket.
    auto fullFilterExpression =
        generateReopeningFilters(time,
                                 normalizedMetadata ? normalizedMetadata->firstElement() : metadata,
                                 controlMinTimePath,
                                 *options.getBucketMaxSpanSeconds());

    DBDirectClient client(opCtx);
    return client.findOne(bucketNss, fullFilterExpression);
}

TEST_F(BucketCatalogHelpersTest, GenerateMinMaxBadBucketDocumentsTest) {
    ASSERT_OK(createCollection(
        operationContext(),
        kNss.dbName(),
        BSON("create" << kNss.coll() << "timeseries" << BSON("timeField" << _timeField))));

    AutoGetCollection autoColl(operationContext(), kNss.makeTimeseriesBucketsNamespace(), MODE_IS);
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
        StatusWith<MinMax> swMinMax = generateMinMaxFromBucketDoc(doc, collator);
        ASSERT_NOT_OK(swMinMax.getStatus());
    }
}

TEST_F(BucketCatalogHelpersTest, GenerateMinMaxTest) {
    ASSERT_OK(createCollection(
        operationContext(),
        kNss.dbName(),
        BSON("create" << kNss.coll() << "timeseries" << BSON("timeField" << _timeField))));

    AutoGetCollection autoColl(operationContext(), kNss.makeTimeseriesBucketsNamespace(), MODE_IS);
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
        StatusWith<MinMax> swMinMax = generateMinMaxFromBucketDoc(doc, collator);
        ASSERT_OK(swMinMax.getStatus());

        MinMax minmax = std::move(swMinMax.getValue());

        ASSERT_BSONOBJ_BINARY_EQ(doc.getObjectField("control").getObjectField("min"), minmax.min());
        ASSERT_BSONOBJ_BINARY_EQ(doc.getObjectField("control").getObjectField("max"), minmax.max());
    }
}

TEST_F(BucketCatalogHelpersTest, GenerateMinMaxWithLowerCaseFirstCollationTest) {
    ASSERT_OK(createCollection(operationContext(),
                               kNss.dbName(),
                               BSON("create" << kNss.coll() << "timeseries"
                                             << BSON("timeField" << _timeField) << "collation"
                                             << BSON("locale"
                                                     << "en_US"
                                                     << "caseFirst"
                                                     << "lower"))));

    AutoGetCollection autoColl(operationContext(), kNss.makeTimeseriesBucketsNamespace(), MODE_IS);
    const CollatorInterface* collator = autoColl->getDefaultCollator();

    // Lowercase compares less than uppercase with a {caseFirst: "lower"} collator.
    BSONObj doc = ::mongo::fromjson(R"({control: {min: {field: "a"}, max: {field: "A"}}})");

    StatusWith<MinMax> swMinMax = generateMinMaxFromBucketDoc(doc, collator);
    ASSERT_OK(swMinMax.getStatus());

    MinMax minmax = std::move(swMinMax.getValue());

    ASSERT_BSONOBJ_BINARY_EQ(doc.getObjectField("control").getObjectField("min"), minmax.min());
    ASSERT_BSONOBJ_BINARY_EQ(doc.getObjectField("control").getObjectField("max"), minmax.max());
}

TEST_F(BucketCatalogHelpersTest, GenerateMinMaxWithUpperCaseFirstCollationTest) {
    ASSERT_OK(createCollection(operationContext(),
                               kNss.dbName(),
                               BSON("create" << kNss.coll() << "timeseries"
                                             << BSON("timeField" << _timeField) << "collation"
                                             << BSON("locale"
                                                     << "en_US"
                                                     << "caseFirst"
                                                     << "upper"))));

    AutoGetCollection autoColl(operationContext(), kNss.makeTimeseriesBucketsNamespace(), MODE_IS);
    const CollatorInterface* collator = autoColl->getDefaultCollator();

    // Uppercase compares less than lowercase with a {caseFirst: "upper"} collator.
    BSONObj doc = ::mongo::fromjson(R"({control: {min: {field: "A"}, max: {field: "a"}}})");

    StatusWith<MinMax> swMinMax = generateMinMaxFromBucketDoc(doc, collator);
    ASSERT_OK(swMinMax.getStatus());

    MinMax minmax = std::move(swMinMax.getValue());

    ASSERT_BSONOBJ_BINARY_EQ(doc.getObjectField("control").getObjectField("min"), minmax.min());
    ASSERT_BSONOBJ_BINARY_EQ(doc.getObjectField("control").getObjectField("max"), minmax.max());
}

TEST_F(BucketCatalogHelpersTest, GenerateMinMaxSucceedsWithMixedSchemaBucketDocumentTest) {
    ASSERT_OK(createCollection(
        operationContext(),
        kNss.dbName(),
        BSON("create" << kNss.coll() << "timeseries" << BSON("timeField" << _timeField))));

    AutoGetCollection autoColl(operationContext(), kNss.makeTimeseriesBucketsNamespace(), MODE_IS);
    const CollatorInterface* collator = autoColl->getDefaultCollator();

    std::vector<BSONObj> docs = {::mongo::fromjson(R"({control:{min: {a: 1}, max: {a: {}}}})"),
                                 ::mongo::fromjson(R"({control:{min: {a: {}}, max: {a: 1}}})"),
                                 ::mongo::fromjson(R"({control:{min: {a: []}, max: {a: {}}}})"),
                                 ::mongo::fromjson(R"({control:{min: {a: 1}, max: {a: "foo"}}})")};

    for (const BSONObj& doc : docs) {
        StatusWith<MinMax> swMinMax = generateMinMaxFromBucketDoc(doc, collator);
        ASSERT_OK(swMinMax.getStatus());
    }
}

TEST_F(BucketCatalogHelpersTest, GenerateSchemaFailsWithMixedSchemaBucketDocumentTest) {
    ASSERT_OK(createCollection(
        operationContext(),
        kNss.dbName(),
        BSON("create" << kNss.coll() << "timeseries" << BSON("timeField" << _timeField))));

    AutoGetCollection autoColl(operationContext(), kNss.makeTimeseriesBucketsNamespace(), MODE_IS);
    const CollatorInterface* collator = autoColl->getDefaultCollator();

    std::vector<BSONObj> docs = {::mongo::fromjson(R"({control:{min: {a: 1}, max: {a: {}}}})"),
                                 ::mongo::fromjson(R"({control:{min: {a: {}}, max: {a: 1}}})"),
                                 ::mongo::fromjson(R"({control:{min: {a: []}, max: {a: {}}}})"),
                                 ::mongo::fromjson(R"({control:{min: {a: 1}, max: {a: "foo"}}})")};

    for (const BSONObj& doc : docs) {
        StatusWith<Schema> swSchema = generateSchemaFromBucketDoc(doc, collator);
        ASSERT_NOT_OK(swSchema.getStatus());
    }
}

TEST_F(BucketCatalogHelpersTest, GenerateSchemaWithInvalidMeasurementsTest) {
    ASSERT_OK(createCollection(
        operationContext(),
        kNss.dbName(),
        BSON("create" << kNss.coll() << "timeseries" << BSON("timeField" << _timeField))));

    AutoGetCollection autoColl(operationContext(), kNss.makeTimeseriesBucketsNamespace(), MODE_IS);
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
        StatusWith<Schema> swSchema = generateSchemaFromBucketDoc(minMaxDoc, collator);
        ASSERT_OK(swSchema.getStatus());

        Schema schema = std::move(swSchema.getValue());

        auto result = schema.update(measurementDoc, /*metaField=*/boost::none, collator);
        ASSERT(result == Schema::UpdateStatus::Failed);
    }
}

TEST_F(BucketCatalogHelpersTest, GenerateSchemaWithValidMeasurementsTest) {
    ASSERT_OK(createCollection(
        operationContext(),
        kNss.dbName(),
        BSON("create" << kNss.coll() << "timeseries" << BSON("timeField" << _timeField))));

    AutoGetCollection autoColl(operationContext(), kNss.makeTimeseriesBucketsNamespace(), MODE_IS);
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
        StatusWith<Schema> swSchema = generateSchemaFromBucketDoc(minMaxDoc, collator);
        ASSERT_OK(swSchema.getStatus());

        Schema schema = std::move(swSchema.getValue());

        auto result = schema.update(measurementDoc, /*metaField=*/boost::none, collator);
        ASSERT(result == Schema::UpdateStatus::Updated);
    }
}

TEST_F(BucketCatalogHelpersTest, FindSuitableBucketForMeasurements) {
    ASSERT_OK(createCollection(
        operationContext(),
        kNss.dbName(),
        BSON("create" << kNss.coll() << "timeseries"
                      << BSON("timeField" << _timeField << "metaField" << _metaField))));

    AutoGetCollection autoColl(operationContext(), kNss.makeTimeseriesBucketsNamespace(), MODE_IX);
    ASSERT(autoColl->getTimeseriesOptions() && autoColl->getTimeseriesOptions()->getMetaField());

    auto tsOptions = *autoColl->getTimeseriesOptions();
    auto metaFieldName = *tsOptions.getMetaField();

    std::vector<BSONObj> bucketDocs = {mongo::fromjson(
                                           R"({
            "_id":{"$oid":"62e7e6ec27c28d338ab29200"},
            "control":{"version":1,"min":{"_id":1,"time":{"$date":"2021-08-01T11:00:00Z"},"a":1},
                                   "max":{"_id":3,"time":{"$date":"2021-08-01T12:00:00Z"},"a":3},
                       "closed":false},
            "meta":1,
            "data":{"time":{"0":{"$date":"2021-08-01T11:00:00Z"},
                            "1":{"$date":"2021-08-01T11:00:00Z"},
                            "2":{"$date":"2021-08-01T11:00:00Z"}},
                    "a":{"0":1,"1":2,"2":3}}})"),
                                       mongo::fromjson(
                                           R"(
            {"_id":{"$oid":"62e7eee4f33f295800073138"},
            "control":{"version":1,"min":{"_id":7,"time":{"$date":"2022-08-01T12:00:00Z"},"a":1},
                                   "max":{"_id":10,"time":{"$date":"2022-08-01T13:00:00Z"},"a":3}},
            "meta":2,
            "data":{"time":{"0":{"$date":"2022-08-01T12:00:00Z"},
                            "1":{"$date":"2022-08-01T12:00:00Z"},
                            "2":{"$date":"2022-08-01T12:00:00Z"}},
                    "a":{"0":1,"1":2,"2":3}}})"),
                                       mongo::fromjson(
                                           R"({
            "_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"_id":7,"time":{"$date":"2023-08-01T13:00:00Z"},"a":1},
                                   "max":{"_id":10,"time":{"$date":"2023-08-01T14:00:00Z"},"a":3},
                       "closed":false},
            "meta":3,
            "data":{"time":{"0":{"$date":"2023-08-01T13:00:00Z"},
                            "1":{"$date":"2023-08-01T13:00:00Z"},
                            "2":{"$date":"2023-08-01T13:00:00Z"}},
                    "a":{"0":1,"1":2,"2":3}}})")};

    // Insert bucket documents into the system.buckets collection.
    for (const auto& doc : bucketDocs) {
        _insertIntoBucketColl(doc);
    }

    auto time1 = dateFromISOString("2021-08-01T11:30:00Z");
    auto time2 = dateFromISOString("2022-08-01T12:30:00Z");
    auto time3 = dateFromISOString("2023-08-01T13:30:00Z");
    std::vector<BSONObj> docsWithSuitableBuckets = {
        BSON("_id" << 1 << _timeField << time1.getValue() << _metaField << 1),
        BSON("_id" << 2 << _timeField << time2.getValue() << _metaField << 2),
        BSON("_id" << 3 << _timeField << time3.getValue() << _metaField << 3)};

    // Iterate through the measurement documents and verify that we can find a suitable bucket to
    // insert into.
    for (size_t i = 0; i < docsWithSuitableBuckets.size(); ++i) {
        const auto& doc = docsWithSuitableBuckets[i];
        auto result = _findSuitableBucket(
            operationContext(), kNss.makeTimeseriesBucketsNamespace(), tsOptions, doc);
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
            auto result = _findSuitableBucket(
                operationContext(), kNss.makeTimeseriesBucketsNamespace(), tsOptions, doc);
            ASSERT(result.isEmpty());
        }

        // Verify that a document with no meta field is suitable with a valid bucket also missing
        // the meta field.
        auto metalessDoc = BSON("_id" << 4 << _timeField << time3.getValue());
        auto metalessBucket = mongo::fromjson(
            R"({
            "_id":{"$oid":"629e1e680958e279dc29a518"},
            "control":{"version":1,"min":{"_id":7,"time":{"$date":"2023-08-01T13:00:00Z"},"a":1},
                                   "max":{"_id":10,"time":{"$date":"2023-08-01T14:00:00Z"},"a":3},
                       "closed":false},
            "data":{"time":{"0":{"$date":"2023-08-01T13:00:00Z"},
                            "1":{"$date":"2023-08-01T13:00:00Z"},
                            "2":{"$date":"2023-08-01T13:00:00Z"}},
                    "a":{"0":1,"1":2,"2":3}}})");
        _insertIntoBucketColl(metalessBucket);

        auto result = _findSuitableBucket(
            operationContext(), kNss.makeTimeseriesBucketsNamespace(), tsOptions, metalessDoc);
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
        auto result = _findSuitableBucket(
            operationContext(), kNss.makeTimeseriesBucketsNamespace(), tsOptions, doc);
        ASSERT(result.isEmpty());
    }
}

TEST_F(BucketCatalogHelpersTest, IncompatibleBucketsForNewMeasurements) {
    ASSERT_OK(createCollection(
        operationContext(),
        kNss.dbName(),
        BSON("create" << kNss.coll() << "timeseries"
                      << BSON("timeField" << _timeField << "metaField" << _metaField))));

    AutoGetCollection autoColl(operationContext(), kNss.makeTimeseriesBucketsNamespace(), MODE_IX);
    ASSERT(autoColl->getTimeseriesOptions() && autoColl->getTimeseriesOptions()->getMetaField());
    auto tsOptions = *autoColl->getTimeseriesOptions();

    std::vector<BSONObj> bucketDocs = {// control.version indicates bucket is compressed.
                                       mongo::fromjson(
                                           R"({
            "_id":{"$oid":"62e7e6ec27c28d338ab29200"},
            "control":{"version":2,"min":{"_id":1,"time":{"$date":"2021-08-01T11:00:00Z"},"a":1},
                                   "max":{"_id":3,"time":{"$date":"2021-08-01T12:00:00Z"},"a":3}},
            "meta":1,
            "data":{"time":{"0":{"$date":"2021-08-01T11:00:00Z"},
                            "1":{"$date":"2021-08-01T11:00:00Z"},
                            "2":{"$date":"2021-08-01T11:00:00Z"}},
                    "a":{"0":1,"1":2,"2":3}}})"),
                                       // control.closed flag is true.
                                       mongo::fromjson(
                                           R"(
            {"_id":{"$oid":"62e7eee4f33f295800073138"},
            "control":{"version":1,"min":{"_id":7,"time":{"$date":"2022-08-01T12:00:00Z"},"a":1},
                                   "max":{"_id":10,"time":{"$date":"2022-08-01T13:00:00Z"},"a":3},
                       "closed":true},
            "meta":2,
            "data":{"time":{"0":{"$date":"2022-08-01T12:00:00Z"},
                            "1":{"$date":"2022-08-01T12:00:00Z"},
                            "2":{"$date":"2022-08-01T12:00:00Z"}},
                    "a":{"0":1,"1":2,"2":3}}})"),
                                       // Compressed bucket with closed flag set.
                                       mongo::fromjson(
                                           R"({
            "_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":2,"min":{"_id":7,"time":{"$date":"2023-08-01T13:00:00Z"},"a":1},
                                   "max":{"_id":10,"time":{"$date":"2023-08-01T14:00:00Z"},"a":3},
                       "closed":true},
            "meta":3,
            "data":{"time":{"0":{"$date":"2023-08-01T13:00:00Z"},
                            "1":{"$date":"2023-08-01T13:00:00Z"},
                            "2":{"$date":"2023-08-01T13:00:00Z"}},
                    "a":{"0":1,"1":2,"2":3}}})")};

    // Insert bucket documents into the system.buckets collection.
    for (const auto& doc : bucketDocs) {
        _insertIntoBucketColl(doc);
    }

    auto time1 = dateFromISOString("2021-08-01T11:30:00Z");
    auto time2 = dateFromISOString("2022-08-01T12:30:00Z");
    auto time3 = dateFromISOString("2023-08-01T13:30:00Z");
    std::vector<BSONObj> validMeasurementDocs = {
        BSON("_id" << 1 << _timeField << time1.getValue() << _metaField << 1),
        BSON("_id" << 2 << _timeField << time2.getValue() << _metaField << 2),
        BSON("_id" << 3 << _timeField << time3.getValue() << _metaField << 3)};

    // Verify that even with matching meta fields and buckets with acceptable time ranges, if the
    // bucket is compressed and/or closed, we should not see it as a candid bucket for future
    // inserts.
    for (const auto& doc : validMeasurementDocs) {
        auto result = _findSuitableBucket(
            operationContext(), kNss.makeTimeseriesBucketsNamespace(), tsOptions, doc);
        ASSERT(result.isEmpty());
    }
}

TEST_F(BucketCatalogHelpersTest, FindDocumentFromOID) {
    ASSERT_OK(createCollection(
        operationContext(),
        kNss.dbName(),
        BSON("create" << kNss.coll() << "timeseries"
                      << BSON("timeField" << _timeField << "metaField" << _metaField))));

    AutoGetCollection autoColl(operationContext(), kNss.makeTimeseriesBucketsNamespace(), MODE_IX);
    ASSERT(autoColl->getTimeseriesOptions() && autoColl->getTimeseriesOptions()->getMetaField());

    std::vector<BSONObj> bucketDocs = {mongo::fromjson(
                                           R"({
            "_id":{"$oid":"62e7e6ec27c28d338ab29200"},
            "control":{"version":1,"min":{"_id":1,"time":{"$date":"2021-08-01T11:00:00Z"},"a":1},
                                   "max":{"_id":3,"time":{"$date":"2021-08-01T12:00:00Z"},"a":3},
                       "closed":false},
            "meta":1,
            "data":{"time":{"0":{"$date":"2021-08-01T11:00:00Z"},
                            "1":{"$date":"2021-08-01T11:00:00Z"},
                            "2":{"$date":"2021-08-01T11:00:00Z"}},
                    "a":{"0":1,"1":2,"2":3}}})"),
                                       mongo::fromjson(
                                           R"(
            {"_id":{"$oid":"62e7eee4f33f295800073138"},
            "control":{"version":1,"min":{"_id":7,"time":{"$date":"2022-08-01T12:00:00Z"},"a":1},
                                   "max":{"_id":10,"time":{"$date":"2022-08-01T13:00:00Z"},"a":3}},
            "meta":2,
            "data":{"time":{"0":{"$date":"2022-08-01T12:00:00Z"},
                            "1":{"$date":"2022-08-01T12:00:00Z"},
                            "2":{"$date":"2022-08-01T12:00:00Z"}},
                    "a":{"0":1,"1":2,"2":3}}})"),
                                       mongo::fromjson(
                                           R"({
            "_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"_id":7,"time":{"$date":"2023-08-01T13:00:00Z"},"a":1},
                                   "max":{"_id":10,"time":{"$date":"2023-08-01T14:00:00Z"},"a":3},
                       "closed":false},
            "meta":3,
            "data":{"time":{"0":{"$date":"2023-08-01T13:00:00Z"},
                            "1":{"$date":"2023-08-01T13:00:00Z"},
                            "2":{"$date":"2023-08-01T13:00:00Z"}},
                    "a":{"0":1,"1":2,"2":3}}})")};

    // Insert bucket documents into the system.buckets collection.
    for (const auto& doc : bucketDocs) {
        _insertIntoBucketColl(doc);
    }

    // Given a valid OID for a bucket document, we should be able to retrieve the full bucket
    // document.
    for (const auto& doc : bucketDocs) {
        const auto bucketId = doc["_id"].OID();
        auto retrievedBucket = findDocFromOID(operationContext(), (*autoColl).get(), bucketId);
        ASSERT(!retrievedBucket.isEmpty());
        ASSERT_BSONOBJ_EQ(retrievedBucket, doc);
    }

    // For non-existent OIDs, we don't expect to retrieve anything.
    std::vector<OID> nonExistentOIDs = {OID("26e7e6ec27c28d338ab29200"),
                                        OID("90e7e6ec27c28d338ab29200"),
                                        OID("00e7e6ec27c28d338ab29200")};
    for (const auto& oid : nonExistentOIDs) {
        auto retrievedBucket = findDocFromOID(operationContext(), (*autoColl).get(), oid);
        ASSERT(retrievedBucket.isEmpty());
    }
}

}  // namespace
}  // namespace mongo::timeseries::bucket_catalog
