/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/ftdc/util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace mongo {

void checkTime(int expected, int now_time, int period) {
    ASSERT_TRUE(Date_t::fromMillisSinceEpoch(expected) ==
                FTDCUtil::roundTime(Date_t::fromMillisSinceEpoch(now_time), Milliseconds(period)));
}

// Validate time rounding
TEST(FTDCUtilTest, TestRoundTime) {
    checkTime(4, 3, 1);
    checkTime(7, 3, 7);
    checkTime(14, 8, 7);
    checkTime(14, 13, 7);
}

// Validate the MongoS FTDC path is computed correctly from a log file path.
TEST(FTDCUtilTest, TestMongoSPath) {

    std::vector<std::pair<std::string, std::string>> testCases = {
        {"/var/log/mongos.log", "/var/log/mongos.diagnostic.data"},
        {"/var/log/mongos.foo.log", "/var/log/mongos.diagnostic.data"},
        {"/var/log/log_file", "/var/log/log_file.diagnostic.data"},
        {"./mongos.log", "./mongos.diagnostic.data"},
        {"../mongos.log", "../mongos.diagnostic.data"},
        {"c:\\var\\log\\mongos.log", "c:\\var\\log\\mongos.diagnostic.data"},
        {"c:\\var\\log\\mongos.foo.log", "c:\\var\\log\\mongos.diagnostic.data"},
        {"c:\\var\\log\\log_file", "c:\\var\\log\\log_file.diagnostic.data"},
        {"/var/some.log/mongos.log", "/var/some.log/mongos.diagnostic.data"},
        {"/var/some.log/log_file", "/var/some.log/log_file.diagnostic.data"},

        {"foo/mongos.log", "foo/mongos.diagnostic.data"},
    };

    for (const auto& p : testCases) {
        ASSERT_EQUALS(FTDCUtil::getMongoSPath(p.first), p.second);
    }
}

TEST(FTDCUtilTest, applyExtractionConversionsToDocumentWorks) {
    BSONObjBuilder refBuilder, expectedBuilder;
    refBuilder.append("field1", 10.5);
    expectedBuilder.append("field1", 10.5);

    refBuilder.append("field2", 1);
    expectedBuilder.append("field2", 1);

    refBuilder.append("field3", INT64_MAX);
    expectedBuilder.append("field3", INT64_MAX);

    refBuilder.append("field4", BSONElement::kLongLongMaxPlusOneAsDouble);
    expectedBuilder.append("field4", INT64_MAX);

    double lessThanInt64Min = -9223372036854777856.0;
    refBuilder.append("field5", lessThanInt64Min);
    expectedBuilder.append("field5", INT64_MIN);

    BSONObjBuilder subObjBuilder1, expectedSubObjBuilder1;
    subObjBuilder1.append("subField1", true);
    expectedSubObjBuilder1.append("subField1", true);

    subObjBuilder1.append("subField2", Date_t::fromMillisSinceEpoch(100000000000000));
    expectedSubObjBuilder1.append("subField2", Date_t::fromMillisSinceEpoch(100000000000000));

    refBuilder.append("field6", subObjBuilder1.obj());
    expectedBuilder.append("field6", expectedSubObjBuilder1.obj());

    BSONObjBuilder subObjBuilder2, expectedSubObjBuilder2;
    subObjBuilder2.append("subField3", Timestamp(100, 10));
    expectedSubObjBuilder2.append("subField3", Timestamp(100, 10));

    subObjBuilder2.append("subField4", std::nan(""));
    expectedSubObjBuilder2.append("subField4", 0);

    refBuilder.appendArray("field7", subObjBuilder2.obj());
    expectedBuilder.appendArray("field7", expectedSubObjBuilder2.obj());

    auto swResultDoc = FTDCBSONUtil::applyExtractionConversionsToDocument(refBuilder.obj());
    ASSERT_OK(swResultDoc);

    BSONObj resultDoc = swResultDoc.getValue();
    ASSERT_BSONOBJ_EQ(resultDoc, expectedBuilder.obj());
}

TEST(FTDCUtilTest, metricUtilsFailAtMaxRecursion) {
    BSONObjBuilder builder1;
    builder1.append("1", 1);
    BSONObjBuilder builder2;
    builder2.append("2", builder1.obj());
    BSONObjBuilder builder3;
    builder3.append("3", builder2.obj());
    BSONObjBuilder builder4;
    builder4.append("4", builder3.obj());
    BSONObjBuilder builder5;
    builder5.append("5", builder4.obj());
    BSONObjBuilder builder6;
    builder6.append("6", builder5.obj());
    BSONObjBuilder builder7;
    builder7.append("7", builder6.obj());
    BSONObjBuilder builder8;
    builder8.append("8", builder7.obj());
    BSONObjBuilder builder9;
    builder9.append("9", builder8.obj());
    BSONObjBuilder builder10;
    builder10.append("10", builder9.obj());
    BSONObjBuilder builder11;
    builder11.append("11", builder10.obj());

    BSONObj okNestedObj = builder11.obj();

    auto swOkConversions = FTDCBSONUtil::applyExtractionConversionsToDocument(okNestedObj);
    ASSERT_OK(swOkConversions);

    std::vector<std::uint64_t> extractionOkMetrics;
    auto swOkExtractions =
        FTDCBSONUtil::extractMetricsFromDocument(okNestedObj, okNestedObj, &extractionOkMetrics);
    ASSERT_OK(swOkExtractions);

    std::vector<std::uint64_t> constructionMetrics = {1};
    auto swOkConstructions =
        FTDCBSONUtil::constructDocumentFromMetrics(okNestedObj, constructionMetrics);
    ASSERT_OK(swOkConstructions);

    BSONObjBuilder builder12;
    builder12.append("12", okNestedObj);

    BSONObj notOkNestedObj = builder12.obj();

    auto swNotOkResultDoc = FTDCBSONUtil::applyExtractionConversionsToDocument(notOkNestedObj);
    ASSERT_NOT_OK(swNotOkResultDoc);

    std::vector<std::uint64_t> extractionNotOkMetrics;
    auto swNotOkExtractions = FTDCBSONUtil::extractMetricsFromDocument(
        notOkNestedObj, notOkNestedObj, &extractionNotOkMetrics);
    ASSERT_NOT_OK(swNotOkExtractions);

    auto swNotOkConstructions =
        FTDCBSONUtil::constructDocumentFromMetrics(notOkNestedObj, constructionMetrics);
    ASSERT_NOT_OK(swNotOkConstructions);
}

TEST(FTDCUtilTest, extractMetricsFromDocumentWorksForSameDoc) {
    BSONObjBuilder refBuilder;
    std::vector<std::uint64_t> expectedMetrics;

    refBuilder.append("field1", 10.5);
    expectedMetrics.push_back(10);

    refBuilder.append("field2", 1);
    expectedMetrics.push_back(1);

    refBuilder.append("field3", INT64_MAX);
    expectedMetrics.push_back(INT64_MAX);

    refBuilder.append("field4", BSONElement::kLongLongMaxPlusOneAsDouble);
    expectedMetrics.push_back(INT64_MAX);

    double lessThanInt64Min = -9223372036854777856.0;
    refBuilder.append("field5", lessThanInt64Min);
    expectedMetrics.push_back(INT64_MIN);

    BSONObjBuilder subObjBuilder1;
    subObjBuilder1.append("subField1", true);
    expectedMetrics.push_back(true);

    subObjBuilder1.append("subField2", Date_t::fromMillisSinceEpoch(100000000000000));
    expectedMetrics.push_back(100000000000000);

    refBuilder.append("field6", subObjBuilder1.obj());

    BSONObjBuilder subObjBuilder2;
    subObjBuilder2.append("subField3", Timestamp(100, 10));
    expectedMetrics.push_back(100);
    expectedMetrics.push_back(10);

    subObjBuilder2.append("subField4", std::nan(""));
    expectedMetrics.push_back(0);

    refBuilder.appendArray("field7", subObjBuilder2.obj());

    BSONObj refObj = refBuilder.obj();

    std::vector<std::uint64_t> actualMetrics;
    auto swMatchesReference =
        FTDCBSONUtil::extractMetricsFromDocument(refObj, refObj, &actualMetrics);

    ASSERT_OK(swMatchesReference);
    ASSERT_TRUE(swMatchesReference.getValue());
    ASSERT_EQ(actualMetrics, expectedMetrics);
}

TEST(FTDCUtilTest, extractMetricsFromDocumentWorksForChangingMetrics) {
    BSONObjBuilder refBuilder, docBuilder;
    std::vector<std::uint64_t> expectedMetrics;

    refBuilder.append("field1", 1);
    docBuilder.append("field1", 2);
    expectedMetrics.push_back(2);

    refBuilder.append("field2", 10.5);
    docBuilder.append("field2", 11.5);
    expectedMetrics.push_back(11);

    std::vector<std::uint64_t> actualMetrics;
    auto swMatchesReference = FTDCBSONUtil::extractMetricsFromDocument(
        refBuilder.obj(), docBuilder.obj(), &actualMetrics);

    ASSERT_OK(swMatchesReference);
    ASSERT_TRUE(swMatchesReference.getValue());
    ASSERT_EQ(actualMetrics, expectedMetrics);
}

TEST(FTDCUtilTest, extractMetricsFromDocumentDocumentsDoNotMatch) {
    std::vector<std::uint64_t> metrics;
    BSONObjBuilder refBuilder1, docBuilder1;

    refBuilder1.append("fieldName", 1);
    docBuilder1.append("differentFieldName", 1);

    auto swMatchesReference1 =
        FTDCBSONUtil::extractMetricsFromDocument(refBuilder1.obj(), docBuilder1.obj(), &metrics);
    ASSERT_OK(swMatchesReference1);
    ASSERT_FALSE(swMatchesReference1.getValue());

    BSONObjBuilder refBuilder2, docBuilder2;

    refBuilder2.append("fieldName", 1);
    docBuilder2.append("fieldName", 1);
    docBuilder2.append("extraFieldName", 1);

    metrics.clear();
    auto swMatchesReference2 =
        FTDCBSONUtil::extractMetricsFromDocument(refBuilder2.obj(), docBuilder2.obj(), &metrics);
    ASSERT_OK(swMatchesReference2);
    ASSERT_FALSE(swMatchesReference2.getValue());

    BSONObjBuilder refBuilder3, docBuilder3;

    refBuilder3.append("shouldBeIntFieldName", 1);
    docBuilder3.append("shouldBeIntFieldName", Timestamp(100, 10));

    metrics.clear();
    auto swMatchesReference3 =
        FTDCBSONUtil::extractMetricsFromDocument(refBuilder3.obj(), docBuilder3.obj(), &metrics);
    ASSERT_OK(swMatchesReference3);
    ASSERT_FALSE(swMatchesReference3.getValue());

    BSONObjBuilder refBuilder4, docBuilder4;

    refBuilder4.append("fieldName", 1);
    refBuilder4.append("extraFieldName", 1);
    docBuilder4.append("fieldName", 1);

    metrics.clear();
    auto swMatchesReference4 =
        FTDCBSONUtil::extractMetricsFromDocument(refBuilder4.obj(), docBuilder4.obj(), &metrics);
    ASSERT_OK(swMatchesReference4);
    ASSERT_FALSE(swMatchesReference4.getValue());
}

TEST(FTDCUtilTest, constructDocumentFromMetricsWorks) {
    BSONObjBuilder refBuilder, expectedObjBuilder;
    std::vector<std::uint64_t> metrics;

    metrics.push_back(10);
    refBuilder.append("field1", 10.5);
    expectedObjBuilder.append("field1", 10);

    metrics.push_back(1);
    refBuilder.append("field2", 1);
    expectedObjBuilder.append("field2", 1);

    metrics.push_back(INT64_MAX);
    refBuilder.append("field3", INT64_MAX);
    expectedObjBuilder.append("field3", INT64_MAX);

    metrics.push_back(INT64_MAX);
    refBuilder.append("field4", BSONElement::kLongLongMaxPlusOneAsDouble);
    expectedObjBuilder.append("field4", INT64_MAX);

    double lessThanInt64Min = -9223372036854777856.0;
    metrics.push_back(INT64_MIN);
    refBuilder.append("field5", lessThanInt64Min);
    expectedObjBuilder.append("field5", INT64_MIN);

    BSONObjBuilder subObjBuilder1;
    metrics.push_back(true);
    subObjBuilder1.append("subField1", true);

    metrics.push_back(100000000000000);
    subObjBuilder1.append("subField2", Date_t::fromMillisSinceEpoch(100000000000000));

    BSONObj subObj1 = subObjBuilder1.obj();

    refBuilder.append("field6", subObj1);
    expectedObjBuilder.append("field6", subObj1);

    BSONObjBuilder subObjBuilder2, expectedSubObjBuilder2;
    metrics.push_back(100);
    metrics.push_back(10);
    subObjBuilder2.append("subField3", Timestamp(100, 10));
    expectedSubObjBuilder2.append("subField3", Timestamp(100, 10));

    metrics.push_back(0);
    subObjBuilder2.append("subField4", std::nan(""));
    expectedSubObjBuilder2.append("subField4", 0);

    refBuilder.appendArray("field7", subObjBuilder2.obj());
    expectedObjBuilder.appendArray("field7", expectedSubObjBuilder2.obj());

    auto swActualDoc = FTDCBSONUtil::constructDocumentFromMetrics(refBuilder.obj(), metrics);

    ASSERT_OK(swActualDoc);
    ASSERT_BSONOBJ_EQ(swActualDoc.getValue(), expectedObjBuilder.obj());
}

TEST(FTDCUtilTest, constructDocumentFromMetricsDocumentAndMetricsDoNotMatch) {
    BSONObjBuilder refBuilder1;
    std::vector<std::uint64_t> metrics1;

    refBuilder1.append("intField", 1);
    refBuilder1.append("extraIntField", 1);
    metrics1.push_back(1);

    auto swObj1 = FTDCBSONUtil::constructDocumentFromMetrics(refBuilder1.obj(), metrics1);
    ASSERT_NOT_OK(swObj1);

    BSONObjBuilder refBuilder2;
    std::vector<std::uint64_t> metrics2;

    refBuilder2.append("boolField", true);
    refBuilder2.append("extraBoolField", false);
    metrics2.push_back(true);

    auto swObj2 = FTDCBSONUtil::constructDocumentFromMetrics(refBuilder2.obj(), metrics2);
    ASSERT_NOT_OK(swObj2);

    BSONObjBuilder refBuilder3;
    std::vector<std::uint64_t> metrics3;

    refBuilder3.append("dateField", Date_t::fromMillisSinceEpoch(100000000000000));
    refBuilder3.append("extraDateField", Date_t::fromMillisSinceEpoch(100000000000001));
    metrics3.push_back(100000000000000);

    auto swObj3 = FTDCBSONUtil::constructDocumentFromMetrics(refBuilder3.obj(), metrics3);
    ASSERT_NOT_OK(swObj3);

    BSONObjBuilder refBuilder4;
    std::vector<std::uint64_t> metrics4;

    refBuilder4.append("timestamp", Timestamp(100, 10));
    refBuilder4.append("extraTimestamp", Timestamp(101, 11));
    metrics4.push_back(100);
    metrics4.push_back(10);

    auto swObj4 = FTDCBSONUtil::constructDocumentFromMetrics(refBuilder4.obj(), metrics4);
    ASSERT_NOT_OK(swObj4);
}

}  // namespace mongo
