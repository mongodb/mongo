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
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/timeseries/bucket_catalog_helpers.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString("test.ts");

class BucketCatalogHelpersTest : public CatalogTestFixture {};

TEST_F(BucketCatalogHelpersTest, GenerateMinMaxBadBucketDocumentsTest) {
    ASSERT_OK(createCollection(operationContext(),
                               kNss.db().toString(),
                               BSON("create" << kNss.coll() << "timeseries"
                                             << BSON("timeField"
                                                     << "time"))));

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
        StatusWith<timeseries::MinMax> swMinMax =
            timeseries::generateMinMaxFromBucketDoc(doc, collator);
        ASSERT_NOT_OK(swMinMax.getStatus());
    }
}

TEST_F(BucketCatalogHelpersTest, GenerateMinMaxTest) {
    ASSERT_OK(createCollection(operationContext(),
                               kNss.db().toString(),
                               BSON("create" << kNss.coll() << "timeseries"
                                             << BSON("timeField"
                                                     << "time"))));

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
        StatusWith<timeseries::MinMax> swMinMax =
            timeseries::generateMinMaxFromBucketDoc(doc, collator);
        ASSERT_OK(swMinMax.getStatus());

        timeseries::MinMax minmax = std::move(swMinMax.getValue());

        ASSERT_BSONOBJ_BINARY_EQ(doc.getObjectField("control").getObjectField("min"), minmax.min());
        ASSERT_BSONOBJ_BINARY_EQ(doc.getObjectField("control").getObjectField("max"), minmax.max());
    }
}

TEST_F(BucketCatalogHelpersTest, GenerateMinMaxWithLowerCaseFirstCollationTest) {
    ASSERT_OK(createCollection(operationContext(),
                               kNss.db().toString(),
                               BSON("create" << kNss.coll() << "timeseries"
                                             << BSON("timeField"
                                                     << "time")
                                             << "collation"
                                             << BSON("locale"
                                                     << "en_US"
                                                     << "caseFirst"
                                                     << "lower"))));

    AutoGetCollection autoColl(operationContext(), kNss.makeTimeseriesBucketsNamespace(), MODE_IS);
    const CollatorInterface* collator = autoColl->getDefaultCollator();

    // Lowercase compares less than uppercase with a {caseFirst: "lower"} collator.
    BSONObj doc = ::mongo::fromjson(R"({control: {min: {field: "a"}, max: {field: "A"}}})");

    StatusWith<timeseries::MinMax> swMinMax =
        timeseries::generateMinMaxFromBucketDoc(doc, collator);
    ASSERT_OK(swMinMax.getStatus());

    timeseries::MinMax minmax = std::move(swMinMax.getValue());

    ASSERT_BSONOBJ_BINARY_EQ(doc.getObjectField("control").getObjectField("min"), minmax.min());
    ASSERT_BSONOBJ_BINARY_EQ(doc.getObjectField("control").getObjectField("max"), minmax.max());
}

TEST_F(BucketCatalogHelpersTest, GenerateMinMaxWithUpperCaseFirstCollationTest) {
    ASSERT_OK(createCollection(operationContext(),
                               kNss.db().toString(),
                               BSON("create" << kNss.coll() << "timeseries"
                                             << BSON("timeField"
                                                     << "time")
                                             << "collation"
                                             << BSON("locale"
                                                     << "en_US"
                                                     << "caseFirst"
                                                     << "upper"))));

    AutoGetCollection autoColl(operationContext(), kNss.makeTimeseriesBucketsNamespace(), MODE_IS);
    const CollatorInterface* collator = autoColl->getDefaultCollator();

    // Uppercase compares less than lowercase with a {caseFirst: "upper"} collator.
    BSONObj doc = ::mongo::fromjson(R"({control: {min: {field: "A"}, max: {field: "a"}}})");

    StatusWith<timeseries::MinMax> swMinMax =
        timeseries::generateMinMaxFromBucketDoc(doc, collator);
    ASSERT_OK(swMinMax.getStatus());

    timeseries::MinMax minmax = std::move(swMinMax.getValue());

    ASSERT_BSONOBJ_BINARY_EQ(doc.getObjectField("control").getObjectField("min"), minmax.min());
    ASSERT_BSONOBJ_BINARY_EQ(doc.getObjectField("control").getObjectField("max"), minmax.max());
}

TEST_F(BucketCatalogHelpersTest, GenerateMinMaxSucceedsWithMixedSchemaBucketDocumentTest) {
    ASSERT_OK(createCollection(operationContext(),
                               kNss.db().toString(),
                               BSON("create" << kNss.coll() << "timeseries"
                                             << BSON("timeField"
                                                     << "time"))));

    AutoGetCollection autoColl(operationContext(), kNss.makeTimeseriesBucketsNamespace(), MODE_IS);
    const CollatorInterface* collator = autoColl->getDefaultCollator();

    std::vector<BSONObj> docs = {::mongo::fromjson(R"({control:{min: {a: 1}, max: {a: {}}}})"),
                                 ::mongo::fromjson(R"({control:{min: {a: {}}, max: {a: 1}}})"),
                                 ::mongo::fromjson(R"({control:{min: {a: []}, max: {a: {}}}})"),
                                 ::mongo::fromjson(R"({control:{min: {a: 1}, max: {a: "foo"}}})")};

    for (const BSONObj& doc : docs) {
        StatusWith<timeseries::MinMax> swMinMax =
            timeseries::generateMinMaxFromBucketDoc(doc, collator);
        ASSERT_OK(swMinMax.getStatus());
    }
}

TEST_F(BucketCatalogHelpersTest, GenerateSchemaFailsWithMixedSchemaBucketDocumentTest) {
    ASSERT_OK(createCollection(operationContext(),
                               kNss.db().toString(),
                               BSON("create" << kNss.coll() << "timeseries"
                                             << BSON("timeField"
                                                     << "time"))));

    AutoGetCollection autoColl(operationContext(), kNss.makeTimeseriesBucketsNamespace(), MODE_IS);
    const CollatorInterface* collator = autoColl->getDefaultCollator();

    std::vector<BSONObj> docs = {::mongo::fromjson(R"({control:{min: {a: 1}, max: {a: {}}}})"),
                                 ::mongo::fromjson(R"({control:{min: {a: {}}, max: {a: 1}}})"),
                                 ::mongo::fromjson(R"({control:{min: {a: []}, max: {a: {}}}})"),
                                 ::mongo::fromjson(R"({control:{min: {a: 1}, max: {a: "foo"}}})")};

    for (const BSONObj& doc : docs) {
        StatusWith<timeseries::Schema> swSchema =
            timeseries::generateSchemaFromBucketDoc(doc, collator);
        ASSERT_NOT_OK(swSchema.getStatus());
    }
}

TEST_F(BucketCatalogHelpersTest, GenerateSchemaWithInvalidMeasurementsTest) {
    ASSERT_OK(createCollection(operationContext(),
                               kNss.db().toString(),
                               BSON("create" << kNss.coll() << "timeseries"
                                             << BSON("timeField"
                                                     << "time"))));

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
        StatusWith<timeseries::Schema> swSchema =
            timeseries::generateSchemaFromBucketDoc(minMaxDoc, collator);
        ASSERT_OK(swSchema.getStatus());

        timeseries::Schema schema = std::move(swSchema.getValue());

        auto result = schema.update(measurementDoc, /*metaField=*/boost::none, collator);
        ASSERT(result == timeseries::Schema::UpdateStatus::Failed);
    }
}

TEST_F(BucketCatalogHelpersTest, GenerateSchemaWithValidMeasurementsTest) {
    ASSERT_OK(createCollection(operationContext(),
                               kNss.db().toString(),
                               BSON("create" << kNss.coll() << "timeseries"
                                             << BSON("timeField"
                                                     << "time"))));

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
        StatusWith<timeseries::Schema> swSchema =
            timeseries::generateSchemaFromBucketDoc(minMaxDoc, collator);
        ASSERT_OK(swSchema.getStatus());

        timeseries::Schema schema = std::move(swSchema.getValue());

        auto result = schema.update(measurementDoc, /*metaField=*/boost::none, collator);
        ASSERT(result == timeseries::Schema::UpdateStatus::Updated);
    }
}

}  // namespace
}  // namespace mongo
