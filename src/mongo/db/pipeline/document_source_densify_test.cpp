/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/document_source_densify.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using GenClass = DocumentSourceInternalDensify::DocGenerator;

Date_t makeDate(std::string dateStr) {
    auto statusDate = dateFromISOString(dateStr);
    ASSERT_TRUE(statusDate.isOK());
    return statusDate.getValue();
}
DEATH_TEST(DensifyGeneratorTest, ErrorsIfMinOverMax, "lower or equal to max") {
    Document doc{{"a", 1}};
    ASSERT_THROWS_CODE(
        GenClass(1, 0, {1, boost::none}, "path", doc, doc), AssertionException, 5733303);
}

DEATH_TEST(DensifyGeneratorTest, ErrorsIfStepIsZero, "be positive") {
    Document doc{{"a", 1}};
    ASSERT_THROWS_CODE(GenClass(0, 1, {0, boost::none, boost::none}, "path", doc, doc),
                       AssertionException,
                       5733305);
}

DEATH_TEST(DensifyGeneratorTest, ErrorsOnMixedValues, "same type") {
    Document doc{{"a", 1}};
    ASSERT_THROWS_CODE(GenClass(0, Date_t::max(), {1, boost::none, boost::none}, "path", doc, doc),
                       AssertionException,
                       5733300);
}

DEATH_TEST(DensifyGeneratorTest, ErrorsIfFieldExistsInDocument, "cannot include field") {
    Document doc{{"path", 1}};
    ASSERT_THROWS_CODE(GenClass(0, 1, {1, boost::none, boost::none}, "path", doc, doc),
                       AssertionException,
                       5733306);
}

DEATH_TEST(DensifyGeneratorTest, ErrorsIfFieldExistsButIsArray, "cannot include field") {
    Document doc{{"path", 1}};
    std::vector<Document> docArray;
    docArray.push_back(doc);
    docArray.push_back(doc);
    Document preservedFields{{"arr", Value(docArray)}};
    ASSERT_THROWS_CODE(GenClass(0, 1, {1, boost::none, boost::none}, "arr", preservedFields, doc),
                       AssertionException,
                       5733306);
}

TEST(DensifyGeneratorTest, ErrorsIfFieldIsInArray) {
    Document doc{{"path", 1}};
    std::vector<Document> docArray;
    docArray.push_back(doc);
    docArray.push_back(doc);
    Document preservedFields{{"arr", Value(docArray)}};
    ASSERT_THROWS_CODE(
        GenClass(0, 1, {1, boost::none, boost::none}, "arr.path", preservedFields, doc),
        AssertionException,
        5733307);
}

TEST(DensifyGeneratorTest, ErrorsIfPrefixOfFieldExists) {
    Document doc{{"a", 2}};
    ASSERT_THROWS_CODE(GenClass(1, 1, {1, boost::none, boost::none}, "a.b", doc, doc),
                       AssertionException,
                       5733308);
}

TEST(DensifyGeneratorTest, GeneratesNumericDocumentCorrectly) {
    Document doc{{"a", 2}};
    auto generator = GenClass(1, 1, {1, boost::none, boost::none}, "a", Document(), doc);
    ASSERT_FALSE(generator.done());
    Document docOne{{"a", 1}};
    ASSERT_DOCUMENT_EQ(docOne, generator.getNextDocument());
    ASSERT_FALSE(generator.done());
    ASSERT_DOCUMENT_EQ(doc, generator.getNextDocument());
    ASSERT_TRUE(generator.done());
}

TEST(DensifyGeneratorTest, GeneratesNumericDocumentCorrectlyWithoutFinalDoc) {
    auto generator = GenClass(1, 1, {1, boost::none, boost::none}, "a", Document(), boost::none);
    ASSERT_FALSE(generator.done());
    Document docOne{{"a", 1}};
    ASSERT_DOCUMENT_EQ(docOne, generator.getNextDocument());
    ASSERT_TRUE(generator.done());
}

TEST(DensifyGeneratorTest, PreservesIncludeFields) {
    Document doc{{"a", 2}, {"b", 2}, {"c", 2}};
    Document preserveFields{{"b", 1}, {"c", 1}};
    auto generator = GenClass(1, 1, {1, boost::none, boost::none}, "a", preserveFields, doc);
    ASSERT_FALSE(generator.done());
    Document docOne{{"b", 1}, {"c", 1}, {"a", 1}};
    ASSERT_DOCUMENT_EQ(docOne, generator.getNextDocument());
    ASSERT_FALSE(generator.done());
    ASSERT_DOCUMENT_EQ(doc, generator.getNextDocument());
    ASSERT_TRUE(generator.done());
}

TEST(DensifyGeneratorTest, GeneratesNumberOfNumericDocumentsCorrectly) {
    Document doc{{"a", 83}};
    auto generator = GenClass(0, 10, {2, boost::none, boost::none}, "a", Document(), doc);
    for (int curVal = 0; curVal <= 10; curVal += 2) {
        ASSERT_FALSE(generator.done());
        Document nextDoc{{"a", curVal}};
        ASSERT_DOCUMENT_EQ(nextDoc, generator.getNextDocument());
    }
    ASSERT_FALSE(generator.done());
    ASSERT_DOCUMENT_EQ(doc, generator.getNextDocument());
    ASSERT_TRUE(generator.done());
}

TEST(DensifyGeneratorTest, WorksWithNonIntegerStepAndPreserveFields) {
    Document doc{{"a", 2}, {"b", 2}, {"c", 2}};
    Document preserveFields{{"b", 1}, {"c", 1}};
    auto generator = GenClass(0, 10, {1.3, boost::none, boost::none}, "a", preserveFields, doc);
    for (double curVal = 0; curVal <= 10; curVal += 1.3) {
        ASSERT_FALSE(generator.done());
        Document nextDoc{{"b", 1}, {"c", 1}, {"a", curVal}};
        ASSERT_DOCUMENT_EQ(nextDoc, generator.getNextDocument());
    }
    ASSERT_FALSE(generator.done());
    ASSERT_DOCUMENT_EQ(doc, generator.getNextDocument());
    ASSERT_TRUE(generator.done());
}

TEST(DensifyGeneratorTest, GeneratesOffsetFromMaxDocsCorrectly) {
    Document doc{{"a", 83}};
    auto generator = GenClass(1, 11, {2, boost::none, boost::none}, "a", Document(), doc);
    for (int curVal = 1; curVal <= 11; curVal += 2) {
        ASSERT_FALSE(generator.done());
        Document nextDoc{{"a", curVal}};
        ASSERT_DOCUMENT_EQ(nextDoc, generator.getNextDocument());
    }
    ASSERT_FALSE(generator.done());
    ASSERT_DOCUMENT_EQ(doc, generator.getNextDocument());
    ASSERT_TRUE(generator.done());
}

TEST(DensifyGeneratorTest, GeneratesAtDottedPathCorrectly) {
    Document doc{{"a", 83}};
    Document preservedFields{{"a", Document{{"b", 1}}}};
    auto generator = GenClass(1, 11, {2, boost::none, boost::none}, "a.c", preservedFields, doc);
    for (int curVal = 1; curVal <= 11; curVal += 2) {
        ASSERT_FALSE(generator.done());
        Document nextDoc{{"a", Document{{"b", 1}, {"c", curVal}}}};
        ASSERT_DOCUMENT_EQ(nextDoc, generator.getNextDocument());
    }
    ASSERT_FALSE(generator.done());
    ASSERT_DOCUMENT_EQ(doc, generator.getNextDocument());
    ASSERT_TRUE(generator.done());
    // Test deeply nested fields.
    Document secondPreservedFields{{"a", Document{{"b", 1}}}};
    generator = GenClass(1, 11, {2, boost::none, boost::none}, "a.c.d", secondPreservedFields, doc);
    for (int curVal = 1; curVal <= 11; curVal += 2) {
        ASSERT_FALSE(generator.done());
        Document nextDoc{{"a", Document{{"b", 1}, {"c", Document{{"d", curVal}}}}}};
        ASSERT_DOCUMENT_EQ(nextDoc, generator.getNextDocument());
    }
    ASSERT_FALSE(generator.done());
    ASSERT_DOCUMENT_EQ(doc, generator.getNextDocument());
    ASSERT_TRUE(generator.done());
}

DEATH_TEST(DensifyGeneratorTest, FailsIfDatesAndTZNotProvided, "time zone") {
    ASSERT_THROWS_CODE(GenClass(makeDate("2021-01-01T00:00:00.000Z"),
                                makeDate("2021-01-01T00:00:02.000Z"),
                                {1, TimeUnit::second, boost::none},
                                "a",
                                Document(),
                                Document()),
                       AssertionException,
                       5733504);
}

DEATH_TEST(DensifyGeneratorTest, FailsIfDatesAndUnitNotProvided, "date step") {
    ASSERT_THROWS_CODE(GenClass(makeDate("2021-01-01T00:00:00.000Z"),
                                makeDate("2021-01-01T00:00:02.000Z"),
                                {1, boost::none, TimeZoneDatabase::utcZone()},
                                "a",
                                Document(),
                                Document()),
                       AssertionException,
                       5733501);
}

DEATH_TEST(DensifyGeneratorTest, FailsIfNumberAndUnitProvided, "non-date") {
    ASSERT_THROWS_CODE(
        GenClass(1, 10, {1, TimeUnit::second, boost::none}, "a", Document(), Document()),
        AssertionException,
        5733506);
}

DEATH_TEST(DensifyGeneratorTest, FailsIfNumberAndTZProvided, "non-date") {
    ASSERT_THROWS_CODE(
        GenClass(1, 10, {1, boost::none, TimeZoneDatabase::utcZone()}, "a", Document(), Document()),
        AssertionException,
        5733506);
}

DEATH_TEST(DensifyGeneratorTest, DateMinMustBeLessThanMax, "lower or equal to") {
    ASSERT_THROWS_CODE(GenClass(makeDate("2021-01-01T00:00:02.000Z"),
                                makeDate("2021-01-01T00:00:01.000Z"),
                                {1, TimeUnit::second, TimeZoneDatabase::utcZone()},
                                "a",
                                Document(),
                                Document()),
                       AssertionException,
                       5733502);
}

DEATH_TEST(DensifyGeneratorTest, DateStepMustBeInt, "integer") {
    ASSERT_THROWS_CODE(GenClass(makeDate("2021-01-01T00:00:00.000Z"),
                                makeDate("2021-01-01T00:00:01.000Z"),
                                {1.5, TimeUnit::second, TimeZoneDatabase::utcZone()},
                                "a",
                                Document(),
                                Document()),
                       AssertionException,
                       5733505);
}

TEST(DensifyGeneratorTest, GeneratesDatesBySecondCorrectly) {
    Document doc{{"a", 83}};
    std::string dateBase = "2021-01-01T00:00:";
    auto generator = GenClass(makeDate("2021-01-01T00:00:01.000Z"),
                              makeDate("2021-01-01T00:00:11.00Z"),
                              {2, TimeUnit::second, TimeZoneDatabase::utcZone()},
                              "a",
                              Document(),
                              doc);
    for (int curVal = 1; curVal <= 11; curVal += 2) {
        auto appendStr = std::to_string(curVal);
        appendStr.insert(appendStr.begin(), 2 - appendStr.length(), '0');
        ASSERT_FALSE(generator.done());
        Document nextDoc{{"a", makeDate(dateBase + appendStr + ".000Z")}};
        ASSERT_DOCUMENT_EQ(nextDoc, generator.getNextDocument());
    }
    ASSERT_FALSE(generator.done());
    ASSERT_DOCUMENT_EQ(doc, generator.getNextDocument());
    ASSERT_TRUE(generator.done());
}

TEST(DensifyGeneratorTest, GeneratesDatesByHourCorrectly) {
    Document doc{{"a", 83}};
    std::string dateBase = "2021-01-01T";
    auto generator = GenClass(makeDate("2021-01-01T01:00:00.000Z"),
                              makeDate("2021-01-01T15:00:00.00Z"),
                              {2, TimeUnit::hour, TimeZoneDatabase::utcZone()},
                              "a",
                              Document(),
                              doc);
    for (int curVal = 1; curVal <= 15; curVal += 2) {
        auto appendStr = std::to_string(curVal);
        appendStr.insert(appendStr.begin(), 2 - appendStr.length(), '0');
        ASSERT_FALSE(generator.done());
        Document nextDoc{{"a", makeDate(dateBase + appendStr + ":00:00.000Z")}};
        ASSERT_DOCUMENT_EQ(nextDoc, generator.getNextDocument());
    }
    ASSERT_FALSE(generator.done());
    ASSERT_DOCUMENT_EQ(doc, generator.getNextDocument());
    ASSERT_TRUE(generator.done());
}

TEST(DensifyGeneratorTest, GeneratesDatesByMonthCorrectly) {
    Document doc{{"a", 83}};
    std::string dateBase = "2021-";
    auto generator = GenClass(makeDate("2021-01-01T01:00:00.000Z"),
                              makeDate("2021-10-01T00:00:00.00Z"),
                              {2, TimeUnit::month, TimeZoneDatabase::utcZone()},
                              "a",
                              Document(),
                              doc);
    for (int curVal = 1; curVal <= 10; curVal += 2) {
        auto appendStr = std::to_string(curVal);
        appendStr.insert(appendStr.begin(), 2 - appendStr.length(), '0');
        ASSERT_FALSE(generator.done());
        Document nextDoc{{"a", makeDate(dateBase + appendStr + "-01T01:00:00.000Z")}};
        ASSERT_DOCUMENT_EQ(nextDoc, generator.getNextDocument());
    }
    ASSERT_FALSE(generator.done());
    ASSERT_DOCUMENT_EQ(doc, generator.getNextDocument());
    ASSERT_TRUE(generator.done());
}
}  // namespace
}  // namespace mongo
