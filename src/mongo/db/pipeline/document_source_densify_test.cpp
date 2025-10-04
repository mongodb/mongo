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

#include "mongo/db/pipeline/document_source_densify.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/agg/densify_stage.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/expression/evaluate.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <boost/none.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

using ExplicitBounds = RangeStatement::ExplicitBounds;
using Full = RangeStatement::Full;
using GenClass = exec::agg::InternalDensifyStage::DocGenerator;
using InternalDensifyStage = exec::agg::InternalDensifyStage;
using DensifyFullNumericTest = AggregationContextFixture;
using DensifyExplicitNumericTest = AggregationContextFixture;
using DensifyMonthStepTest = AggregationContextFixture;
using DensifyPartitionNumericTest = AggregationContextFixture;
using DensifyCloneTest = AggregationContextFixture;
using DensifyStepTest = AggregationContextFixture;
using DensifySpecTest = AggregationContextFixture;
using DensifyRedactionTest = AggregationContextFixture;

Date_t makeDate(std::string dateStr) {
    auto statusDate = dateFromISOString(dateStr);
    ASSERT_TRUE(statusDate.isOK());
    return statusDate.getValue();
}

// Helper to verify that generated values relate correctly to the step.
bool isOnStepRelativeTo(DensifyValue testVal, DensifyValue base, RangeStatement range) {
    if (testVal.isDate()) {

        auto date = testVal.getDate();
        auto unit = range.getUnit().value();
        long long step = range.getStep().coerceToLong();
        auto baseDate = base.getDate();

        // Months, quarters and years have variable lengths depending on leap days
        // and days-in-a-month, so a step is not a constant number of milliseconds
        // across all dates. For these units, we need to iterate through rather
        // than performing a calculation with modulo. As long as `baseDate` is not
        // a large number of steps away from the value we're testing (as is true in
        // our usage with _current as the base), this should not be a performance
        // issue.
        if (unit == TimeUnit::month || unit == TimeUnit::quarter || unit == TimeUnit::year) {

            Date_t steppedDate = baseDate;
            while (steppedDate < date) {
                steppedDate = dateAdd(steppedDate, unit, step, TimeZoneDatabase::utcZone());
            }
            return steppedDate == date;
        } else {
            // Steps with units smaller than one month are always constant sized
            // (because unix time does not have leap seconds), so we can perform
            // modulo arithmetic.
            auto testMillis = date.toMillisSinceEpoch();
            auto baseMillis = baseDate.toMillisSinceEpoch();
            auto stepDurationInMillis =
                dateAdd(Date_t::fromMillisSinceEpoch(0), unit, step, TimeZoneDatabase::utcZone())
                    .toMillisSinceEpoch();
            auto diff = testMillis - baseMillis;
            return diff % stepDurationInMillis == 0;
        }
    } else if (testVal.isNumber()) {
        Value diff = uassertStatusOK(
            exec::expression::evaluateSubtract(testVal.getNumber(), base.getNumber()));
        Value remainder = uassertStatusOK(exec::expression::evaluateMod(diff, range.getStep()));
        return remainder.getDouble() == 0.0;
    }
    return false;
}

DEATH_TEST(DensifyGeneratorTest, ErrorsIfMinOverMax, "lower or equal to max") {
    Document doc{{"a", 1}};
    size_t counter = 0;
    GenClass(Value(1),
             RangeStatement(Value(1), ExplicitBounds(Value(1), Value(0)), boost::none),
             "path",
             doc,
             doc,
             boost::none,
             ValueComparator(),
             &counter,
             false);
}

DEATH_TEST(DensifyGeneratorTest, ErrorsIfStepIsZero, "be positive") {
    Document doc{{"a", 1}};
    size_t counter = 0;
    GenClass(Value(0),
             RangeStatement(Value(0), ExplicitBounds(Value(0), Value(1)), boost::none),
             "path",
             doc,
             doc,
             boost::none,
             ValueComparator(),
             &counter,
             false);
}

DEATH_TEST(DensifyGeneratorTest, ErrorsOnMixedValues, "same type") {
    Document doc{{"a", 1}};
    size_t counter = 0;
    GenClass(Date_t::max(),
             RangeStatement(Value(1), ExplicitBounds(Value(0), Value(1)), boost::none),
             "path",
             doc,
             doc,
             boost::none,
             ValueComparator(),
             &counter,
             false);
}

DEATH_TEST(DensifyGeneratorTest, ErrorsIfFieldExistsInDocument, "cannot include field") {
    Document doc{{"path", 1}};
    size_t counter = 0;
    GenClass(Value(0),
             RangeStatement(Value(1), ExplicitBounds(Value(0), Value(1)), boost::none),
             "path",
             doc,
             doc,
             boost::none,
             ValueComparator(),
             &counter,
             false);
}

DEATH_TEST(DensifyGeneratorTest, ErrorsIfFieldExistsButIsArray, "cannot include field") {
    Document doc{{"path", 1}};
    std::vector<Document> docArray;
    docArray.push_back(doc);
    docArray.push_back(doc);
    Document preservedFields{{"arr", Value(docArray)}};
    size_t counter = 0;
    GenClass(Value(0),
             RangeStatement(Value(1), ExplicitBounds(Value(0), Value(1)), boost::none),
             "arr",
             preservedFields,
             doc,
             boost::none,
             ValueComparator(),
             &counter,
             false);
}

TEST(DensifyGeneratorTest, ErrorsIfFieldIsInArray) {
    Document doc{{"path", 1}};
    std::vector<Document> docArray;
    docArray.push_back(doc);
    docArray.push_back(doc);
    Document preservedFields{{"arr", Value(docArray)}};
    size_t counter = 0;
    ASSERT_THROWS_CODE(
        GenClass(Value(0),
                 RangeStatement(Value(1), ExplicitBounds(Value(0), Value(1)), boost::none),
                 "arr.path",
                 preservedFields,
                 doc,
                 boost::none,
                 ValueComparator(),
                 &counter,
                 false),
        AssertionException,
        5733307);
}

TEST(DensifyGeneratorTest, ErrorsIfPrefixOfFieldExists) {
    Document doc{{"a", 2}};
    size_t counter = 0;
    ASSERT_THROWS_CODE(
        GenClass(Value(1),
                 RangeStatement(Value(1), ExplicitBounds(Value(1), Value(1)), boost::none),
                 "a.b",
                 doc,
                 doc,
                 boost::none,
                 ValueComparator(),
                 &counter,
                 false),
        AssertionException,
        5733308);
}

TEST(DensifyGeneratorTest, GeneratesNumericDocumentCorrectly) {
    Document doc{{"a", 2}};
    size_t counter = 0;
    auto generator =
        GenClass(Value(1),
                 RangeStatement(Value(1), ExplicitBounds(Value(1), Value(1)), boost::none),
                 "a",
                 Document(),
                 doc,
                 boost::none,
                 ValueComparator(),
                 &counter,
                 false);
    ASSERT_FALSE(generator.done());
    Document docOne{{"a", 1}};
    ASSERT_DOCUMENT_EQ(docOne, generator.getNextDocument());
    ASSERT_FALSE(generator.done());
    ASSERT_DOCUMENT_EQ(doc, generator.getNextDocument());
    ASSERT_TRUE(generator.done());
}

TEST(DensifyGeneratorTest, GeneratesNumericDocumentCorrectlyWithoutFinalDoc) {
    size_t counter = 0;
    auto generator =
        GenClass(Value(1),
                 RangeStatement(Value(1), ExplicitBounds(Value(1), Value(1)), boost::none),
                 "a",
                 Document(),
                 boost::none,
                 boost::none,
                 ValueComparator(),
                 &counter,
                 false);
    ASSERT_FALSE(generator.done());
    Document docOne{{"a", 1}};
    ASSERT_DOCUMENT_EQ(docOne, generator.getNextDocument());
    ASSERT_TRUE(generator.done());
}

TEST(DensifyGeneratorTest, PreservesIncludeFields) {
    Document doc{{"a", 2}, {"b", 2}, {"c", 2}};
    Document preserveFields{{"b", 1}, {"c", 1}};
    size_t counter = 0;
    auto generator =
        GenClass(Value(1),
                 RangeStatement(Value(1), ExplicitBounds(Value(1), Value(1)), boost::none),
                 "a",
                 preserveFields,
                 doc,
                 boost::none,
                 ValueComparator(),
                 &counter,
                 false);
    ASSERT_FALSE(generator.done());
    Document docOne{{"b", 1}, {"c", 1}, {"a", 1}};
    ASSERT_DOCUMENT_EQ(docOne, generator.getNextDocument());
    ASSERT_FALSE(generator.done());
    ASSERT_DOCUMENT_EQ(doc, generator.getNextDocument());
    ASSERT_TRUE(generator.done());
}

TEST(DensifyGeneratorTest, GeneratesNumberOfNumericDocumentsCorrectly) {
    Document doc{{"a", 83}};
    size_t counter = 0;
    auto generator =
        GenClass(Value(0),
                 RangeStatement(Value(2), ExplicitBounds(Value(0), Value(10)), boost::none),
                 "a",
                 Document(),
                 doc,
                 boost::none,
                 ValueComparator(),
                 &counter,
                 false);
    for (int curVal = 0; curVal < 10; curVal += 2) {
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
    size_t counter = 0;
    auto generator =
        GenClass(Value(0),
                 RangeStatement(Value(1.3), ExplicitBounds(Value(0), Value(10)), boost::none),
                 "a",
                 preserveFields,
                 doc,
                 boost::none,
                 ValueComparator(),
                 &counter,
                 false);
    for (double curVal = 0; curVal < 10; curVal += 1.3) {
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
    size_t counter = 0;
    auto generator =
        GenClass(Value(1),
                 RangeStatement(Value(2), ExplicitBounds(Value(1), Value(11)), boost::none),
                 "a",
                 Document(),
                 doc,
                 boost::none,
                 ValueComparator(),
                 &counter,
                 false);
    for (int curVal = 1; curVal < 11; curVal += 2) {
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
    size_t counter = 0;
    auto generator =
        GenClass(Value(1),
                 RangeStatement(Value(2), ExplicitBounds(Value(1), Value(11)), boost::none),
                 "a.c",
                 preservedFields,
                 doc,
                 boost::none,
                 ValueComparator(),
                 &counter,
                 false);
    for (int curVal = 1; curVal < 11; curVal += 2) {
        ASSERT_FALSE(generator.done());
        Document nextDoc{{"a", Document{{"b", 1}, {"c", curVal}}}};
        ASSERT_DOCUMENT_EQ(nextDoc, generator.getNextDocument());
    }
    ASSERT_FALSE(generator.done());
    ASSERT_DOCUMENT_EQ(doc, generator.getNextDocument());
    ASSERT_TRUE(generator.done());
    counter = 0;
    // Test deeply nested fields.
    Document secondPreservedFields{{"a", Document{{"b", 1}}}};
    generator = GenClass(Value(1),
                         RangeStatement(Value(2), ExplicitBounds(Value(1), Value(11)), boost::none),
                         "a.c.d",
                         secondPreservedFields,
                         doc,
                         boost::none,
                         ValueComparator(),
                         &counter,
                         false);
    for (int curVal = 1; curVal < 11; curVal += 2) {
        ASSERT_FALSE(generator.done());
        Document nextDoc{{"a", Document{{"b", 1}, {"c", Document{{"d", curVal}}}}}};
        ASSERT_DOCUMENT_EQ(nextDoc, generator.getNextDocument());
    }
    ASSERT_FALSE(generator.done());
    ASSERT_DOCUMENT_EQ(doc, generator.getNextDocument());
    ASSERT_TRUE(generator.done());
}

DEATH_TEST(DensifyGeneratorTest, FailsIfDatesAndUnitNotProvided, "date step") {
    size_t counter = 0;
    GenClass(makeDate("2021-01-01T00:00:00.000Z"),
             RangeStatement(Value(1),
                            ExplicitBounds(makeDate("2021-01-01T00:00:00.000Z"),
                                           makeDate("2021-01-01T00:00:02.000Z")),
                            boost::none),
             "a",
             Document(),
             Document(),
             boost::none,
             ValueComparator(),
             &counter,
             false);
}

DEATH_TEST(DensifyGeneratorTest, FailsIfNumberAndUnitProvided, "non-date") {
    size_t counter = 0;
    GenClass(Value(1),
             RangeStatement(Value(1), ExplicitBounds(Value(1), Value(10)), TimeUnit::second),
             "a",
             Document(),
             Document(),
             boost::none,
             ValueComparator(),
             &counter,
             false);
}

DEATH_TEST(DensifyGeneratorTest, DateMinMustBeLessThanMax, "lower or equal to") {
    size_t counter = 0;
    GenClass(makeDate("2021-01-01T00:00:02.000Z"),
             RangeStatement(Value(1),
                            ExplicitBounds(makeDate("2021-01-01T00:00:02.000Z"),
                                           makeDate("2021-01-01T00:00:01.000Z")),
                            TimeUnit::second),
             "a",
             Document(),
             Document(),
             boost::none,
             ValueComparator(),
             &counter,
             false);
}

DEATH_TEST(DensifyGeneratorTest, DateStepMustBeInt, "whole number") {
    size_t counter = 0;
    GenClass(makeDate("2021-01-01T00:00:00.000Z"),
             RangeStatement(Value(1.5),
                            ExplicitBounds(makeDate("2021-01-01T00:00:00.000Z"),
                                           makeDate("2021-01-01T00:00:01.000Z")),
                            TimeUnit::second),
             "a",
             Document(),
             Document(),
             boost::none,
             ValueComparator(),
             &counter,
             false);
}

TEST(DensifyGeneratorTest, GeneratesDatesBySecondCorrectly) {
    size_t counter = 0;
    Document doc{{"a", 83}};
    std::string dateBase = "2021-01-01T00:00:";
    auto generator = GenClass(makeDate("2021-01-01T00:00:01.000Z"),
                              RangeStatement(Value(2),
                                             ExplicitBounds(makeDate("2021-01-01T00:00:01.000Z"),
                                                            makeDate("2021-01-01T00:00:11.000Z")),
                                             TimeUnit::second),
                              "a",
                              Document(),
                              doc,
                              boost::none,
                              ValueComparator(),
                              &counter,
                              false);
    for (int curVal = 1; curVal < 11; curVal += 2) {
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
    size_t counter = 0;
    std::string dateBase = "2021-01-01T";
    auto generator = GenClass(makeDate("2021-01-01T01:00:00.000Z"),
                              RangeStatement(Value(2),
                                             ExplicitBounds(makeDate("2021-01-01T01:00:00.000Z"),
                                                            makeDate("2021-01-01T15:00:00.000Z")),
                                             TimeUnit::hour),
                              "a",
                              Document(),
                              doc,
                              boost::none,
                              ValueComparator(),
                              &counter,
                              false);
    for (int curVal = 1; curVal < 15; curVal += 2) {
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
    size_t counter = 0;
    Document doc{{"a", 83}};
    std::string dateBase = "2021-";
    auto generator = GenClass(makeDate("2021-01-01T01:00:00.000Z"),
                              RangeStatement(Value(2),
                                             ExplicitBounds(makeDate("2021-01-01T00:00:00.000Z"),
                                                            makeDate("2021-10-01T00:00:00.000Z")),
                                             TimeUnit::month),
                              "a",
                              Document(),
                              doc,
                              boost::none,
                              ValueComparator(),
                              &counter,
                              false);
    for (int curVal = 1; curVal < 10; curVal += 2) {
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
TEST_F(DensifyFullNumericTest, DensifySingleValue) {
    auto densify = InternalDensifyStage(DocumentSourceInternalDensify::kStageName,
                                        getExpCtx(),
                                        "a",
                                        std::list<FieldPath>(),
                                        RangeStatement(Value(2), Full(), boost::none));
    auto stage = exec::agg::MockStage::createForTest({"{a: 1}"}, getExpCtx());
    densify.setSource(stage.get());

    auto next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("a" << 1));

    ASSERT(densify.getNext().isEOF());
}

TEST_F(DensifyFullNumericTest, DensifyValuesCorrectlyWithDuplicates) {
    auto densify = InternalDensifyStage(DocumentSourceInternalDensify::kStageName,
                                        getExpCtx(),
                                        "a",
                                        std::list<FieldPath>(),
                                        RangeStatement(Value(2), Full(), boost::none));
    auto stage = exec::agg::MockStage::createForTest(
        {"{a: 1}", "{a: 1}", "{a: 1}", "{a: 3}", "{a: 7}", "{a: 7}", "{a: 7}"}, getExpCtx());
    densify.setSource(stage.get());

    auto next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("a" << 1));

    next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("a" << 1));

    next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("a" << 1));

    next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("a" << 3));

    next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("a" << 5));

    next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("a" << 7));

    next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("a" << 7));

    next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("a" << 7));
    ASSERT(densify.getNext().isEOF());
}

TEST_F(DensifyFullNumericTest, DensifyValuesCorrectlyOffStep) {
    auto densify = InternalDensifyStage("",
                                        getExpCtx(),
                                        "a",
                                        std::list<FieldPath>(),
                                        RangeStatement(Value(3), Full(), boost::none));
    auto stage = exec::agg::MockStage::createForTest({"{a: 1}", "{a: 9}"}, getExpCtx());
    densify.setSource(stage.get());

    auto next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("a" << 1));

    next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("a" << 4));

    next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("a" << 7));

    next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("a" << 9));

    // Check that multiple EOFs are correctly returned at the end
    ASSERT(densify.getNext().isEOF());
    ASSERT(densify.getNext().isEOF());
    ASSERT(densify.getNext().isEOF());
}

TEST_F(DensifyFullNumericTest, DensifyValuesCorrectlyOnStep) {
    auto densify = InternalDensifyStage(DocumentSourceInternalDensify::kStageName,
                                        getExpCtx(),
                                        "a",
                                        std::list<FieldPath>(),
                                        RangeStatement(Value(2), Full(), boost::none));
    auto stage = exec::agg::MockStage::createForTest({"{a: 1}", "{a: 9}"}, getExpCtx());
    densify.setSource(stage.get());

    auto next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("a" << 1));

    next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("a" << 3));

    next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("a" << 5));

    next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("a" << 7));

    next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("a" << 9));
    ASSERT(densify.getNext().isEOF());

    // Check that multiple EOFs are correctly returned at the end
    ASSERT(densify.getNext().isEOF());
    ASSERT(densify.getNext().isEOF());
    ASSERT(densify.getNext().isEOF());
}

TEST_F(DensifyFullNumericTest,
       NoDensificationIfStepIsGreaterThanDocumentDifferenceMultipleDocuments) {
    auto densify = InternalDensifyStage(DocumentSourceInternalDensify::kStageName,
                                        getExpCtx(),
                                        "a",
                                        std::list<FieldPath>(),
                                        RangeStatement(Value(2), Full(), boost::none));
    auto stage =
        exec::agg::MockStage::createForTest({"{a: 1}", "{a : 2}", "{a: 3}", "{a: 4}"}, getExpCtx());
    densify.setSource(stage.get());

    auto next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("a" << 1));

    next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("a" << 2));

    next = densify.getNext();

    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("a" << 3));

    next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("a" << 4));
    ASSERT(densify.getNext().isEOF());
}

TEST_F(DensifyFullNumericTest, DensificationFieldMissing) {
    auto densify = InternalDensifyStage(DocumentSourceInternalDensify::kStageName,
                                        getExpCtx(),
                                        "a",
                                        std::list<FieldPath>(),
                                        RangeStatement(Value(10), Full(), boost::none));
    auto source = exec::agg::buildStage(DocumentSourceMock::createForTest(
        {"{b: 1}", "{a: 1}", "{a: 20}", "{b: 2}", "{b: 3}"}, getExpCtx()));
    densify.setSource(source.get());

    auto next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("b" << 1));

    next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("a" << 1));

    next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("a" << 11));

    next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("a" << 20));

    next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("b" << 2));

    next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("b" << 3));
    ASSERT(densify.getNext().isEOF());
}

TEST_F(DensifyFullNumericTest, NoDensificationIfStepGreaterThanDocumentDifference) {
    auto densify = InternalDensifyStage(DocumentSourceInternalDensify::kStageName,
                                        getExpCtx(),
                                        "a",
                                        std::list<FieldPath>(),
                                        RangeStatement(Value(10), Full(), boost::none));
    auto stage = exec::agg::MockStage::createForTest({"{a: 1}", "{a: 9}"}, getExpCtx());
    densify.setSource(stage.get());

    auto next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("a" << 1));

    next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("a" << 9));
    ASSERT(densify.getNext().isEOF());
}

TEST_F(DensifyFullNumericTest, DensifyOverDocumentsWithGaps) {
    auto densify = InternalDensifyStage(DocumentSourceInternalDensify::kStageName,
                                        getExpCtx(),
                                        "a",
                                        std::list<FieldPath>(),
                                        RangeStatement(Value(3), Full(), boost::none));
    auto stage = exec::agg::MockStage::createForTest(
        {"{a: 1}", "{a: 2}", "{a : 3}", "{a : 4}", "{a : 9}", "{a : 10}", "{a : 15}"}, getExpCtx());
    densify.setSource(stage.get());

    auto next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("a" << 1));

    next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("a" << 2));

    next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("a" << 3));

    next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("a" << 4));

    next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("a" << 7));

    next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("a" << 9));

    next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("a" << 10));

    next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("a" << 13));

    next = densify.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), DOC("a" << 15));
    ASSERT(densify.getNext().isEOF());
}

TEST_F(DensifyExplicitNumericTest, CorrectlyDensifiesForNumericExplicitRangeStartingBelowRange) {
    auto densify = InternalDensifyStage(
        DocumentSourceInternalDensify::kStageName,
        getExpCtx(),
        "a",
        std::list<FieldPath>(),
        RangeStatement(Value(2), ExplicitBounds(Value(5), Value(15)), boost::none));
    auto stage = exec::agg::MockStage::createForTest(
        {"{a: 0}", "{a: 1}", "{a: 8}", "{a: 13}", "{a: 19}"}, getExpCtx());
    densify.setSource(stage.get());

    auto next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(0, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(1, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(5, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(7, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(8, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(9, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(11, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(13, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(19, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT_FALSE(next.isAdvanced());

    next = densify.getNext();
    ASSERT_FALSE(next.isAdvanced());
}


TEST_F(DensifyExplicitNumericTest,
       CorrectlyDensifiesForNumericExplicitRangeStepStartingOnMinRange) {
    auto densify = InternalDensifyStage(
        DocumentSourceInternalDensify::kStageName,
        getExpCtx(),
        "a",
        std::list<FieldPath>(),
        RangeStatement(Value(1), ExplicitBounds(Value(0), Value(5)), boost::none));
    auto stage = exec::agg::MockStage::createForTest(
        {"{a: 0}", "{a: 1}", "{a: 8}", "{a: 13}", "{a: 19}"}, getExpCtx());
    densify.setSource(stage.get());

    auto next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(0, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(1, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(2, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(3, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(4, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(8, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(13, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(19, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT_FALSE(next.isAdvanced());

    next = densify.getNext();
    ASSERT_FALSE(next.isAdvanced());
}

TEST_F(DensifyExplicitNumericTest, CorrectlyDensifiesForNumericExplicitRangeStartingInsideRange) {
    auto densify = InternalDensifyStage(
        DocumentSourceInternalDensify::kStageName,
        getExpCtx(),
        "a",
        std::list<FieldPath>(),
        RangeStatement(Value(1), ExplicitBounds(Value(0), Value(4)), boost::none));
    auto stage =
        exec::agg::MockStage::createForTest({"{a: 1}", "{a: 2}", "{a: 4}", "{a: 6}"}, getExpCtx());
    densify.setSource(stage.get());

    auto next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(0, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(1, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(2, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(3, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(4, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(6, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT_FALSE(next.isAdvanced());

    next = densify.getNext();
    ASSERT_FALSE(next.isAdvanced());
}

TEST_F(DensifyExplicitNumericTest,
       CorrectlyDensifiesForPartionedRangeStartingBeforeBoundsRangeWithDocMatchingBoundsStart) {
    auto densify = InternalDensifyStage(
        "$_internalDensify",
        getExpCtx(),
        "b",
        std::list<FieldPath>({"a"}),
        RangeStatement(Value(1), ExplicitBounds(Value(4), Value(8)), boost::none));
    auto stage = exec::agg::MockStage::createForTest(
        {"{a: 1, b: 2}", "{a: 1, b: 4}", "{a: 1, b: 6}"}, getExpCtx());
    densify.setSource(stage.get());

    auto next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(2, next.getDocument().getField("b").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(4, next.getDocument().getField("b").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(5, next.getDocument().getField("b").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(6, next.getDocument().getField("b").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(7, next.getDocument().getField("b").getDouble());

    next = densify.getNext();
    ASSERT_FALSE(next.isAdvanced());

    next = densify.getNext();
    ASSERT_FALSE(next.isAdvanced());
}

TEST_F(DensifyExplicitNumericTest, CorrectlyDensifiesForPartionedRangeAcrossTwoPartitions) {
    auto densify = InternalDensifyStage(
        "$_internalDensify",
        getExpCtx(),
        "b",
        std::list<FieldPath>({"a"}),
        RangeStatement(Value(1), ExplicitBounds(Value(0), Value(10)), boost::none));
    auto stage = exec::agg::MockStage::createForTest(
        {"{a: 1, b: 2}", "{a: 2, b: 5}", "{a: 1, b: 7}", "{a: 2, b: 9}"}, getExpCtx());
    densify.setSource(stage.get());

    auto next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(1, next.getDocument().getField("a").getDouble());
    ASSERT_EQUALS(0, next.getDocument().getField("b").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(1, next.getDocument().getField("a").getDouble());
    ASSERT_EQUALS(1, next.getDocument().getField("b").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(1, next.getDocument().getField("a").getDouble());
    ASSERT_EQUALS(2, next.getDocument().getField("b").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(2, next.getDocument().getField("a").getDouble());
    ASSERT_EQUALS(0, next.getDocument().getField("b").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(2, next.getDocument().getField("a").getDouble());
    ASSERT_EQUALS(1, next.getDocument().getField("b").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(2, next.getDocument().getField("a").getDouble());
    ASSERT_EQUALS(2, next.getDocument().getField("b").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(2, next.getDocument().getField("a").getDouble());
    ASSERT_EQUALS(3, next.getDocument().getField("b").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(2, next.getDocument().getField("a").getDouble());
    ASSERT_EQUALS(4, next.getDocument().getField("b").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(2, next.getDocument().getField("a").getDouble());
    ASSERT_EQUALS(5, next.getDocument().getField("b").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(1, next.getDocument().getField("a").getDouble());
    ASSERT_EQUALS(3, next.getDocument().getField("b").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(1, next.getDocument().getField("a").getDouble());
    ASSERT_EQUALS(4, next.getDocument().getField("b").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(1, next.getDocument().getField("a").getDouble());
    ASSERT_EQUALS(5, next.getDocument().getField("b").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(1, next.getDocument().getField("a").getDouble());
    ASSERT_EQUALS(6, next.getDocument().getField("b").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(1, next.getDocument().getField("a").getDouble());
    ASSERT_EQUALS(7, next.getDocument().getField("b").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(2, next.getDocument().getField("a").getDouble());
    ASSERT_EQUALS(6, next.getDocument().getField("b").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(2, next.getDocument().getField("a").getDouble());
    ASSERT_EQUALS(7, next.getDocument().getField("b").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(2, next.getDocument().getField("a").getDouble());
    ASSERT_EQUALS(8, next.getDocument().getField("b").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(2, next.getDocument().getField("a").getDouble());
    ASSERT_EQUALS(9, next.getDocument().getField("b").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(1, next.getDocument().getField("a").getDouble());
    ASSERT_EQUALS(8, next.getDocument().getField("b").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(1, next.getDocument().getField("a").getDouble());
    ASSERT_EQUALS(9, next.getDocument().getField("b").getDouble());

    next = densify.getNext();
    ASSERT_FALSE(next.isAdvanced());

    next = densify.getNext();
    ASSERT_FALSE(next.isAdvanced());
}

TEST_F(DensifyExplicitNumericTest, CorrectlyDensifiesForNumericExplicitRangeOnlyInsideRange) {
    auto densify = InternalDensifyStage(
        DocumentSourceInternalDensify::kStageName,
        getExpCtx(),
        "a",
        std::list<FieldPath>(),
        RangeStatement(Value(1), ExplicitBounds(Value(0), Value(4)), boost::none));
    auto stage = exec::agg::MockStage::createForTest({"{a: 1}"}, getExpCtx());
    densify.setSource(stage.get());

    auto next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(0, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(1, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(2, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(3, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT_FALSE(next.isAdvanced());

    next = densify.getNext();
    ASSERT_FALSE(next.isAdvanced());
}

TEST_F(DensifyExplicitNumericTest, CorrectlyDensifiesForNumericExplicitRangeStartingAboveRange) {
    auto densify = InternalDensifyStage(
        DocumentSourceInternalDensify::kStageName,
        getExpCtx(),
        "a",
        std::list<FieldPath>(),
        RangeStatement(Value(1), ExplicitBounds(Value(0), Value(5)), boost::none));
    auto stage = exec::agg::MockStage::createForTest({"{a: 6}", "{a: 7}"}, getExpCtx());
    densify.setSource(stage.get());

    auto next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(0, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(1, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(2, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(3, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(4, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(6, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(7, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT_FALSE(next.isAdvanced());

    next = densify.getNext();
    ASSERT_FALSE(next.isAdvanced());
}

TEST_F(DensifyExplicitNumericTest, CorrectlyDensifiesForNumericExplicitRangeStartingInsideOffStep) {
    auto densify = InternalDensifyStage(
        DocumentSourceInternalDensify::kStageName,
        getExpCtx(),
        "a",
        std::list<FieldPath>(),
        RangeStatement(Value(2), ExplicitBounds(Value(0), Value(4)), boost::none));
    auto stage = exec::agg::MockStage::createForTest({"{a: 1}", "{a: 6}"}, getExpCtx());
    densify.setSource(stage.get());

    auto next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(0, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(1, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(2, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(6, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT_FALSE(next.isAdvanced());

    next = densify.getNext();
    ASSERT_FALSE(next.isAdvanced());
}

TEST_F(DensifyExplicitNumericTest,
       CorrectlyDensifiesForNumericExplicitRangeStartingInsideWithDupes) {
    auto densify = InternalDensifyStage(
        DocumentSourceInternalDensify::kStageName,
        getExpCtx(),
        "a",
        std::list<FieldPath>(),
        RangeStatement(Value(1), ExplicitBounds(Value(0), Value(4)), boost::none));
    auto stage = exec::agg::MockStage::createForTest({"{a: 1}", "{a: 1}", "{a: 6}"}, getExpCtx());
    densify.setSource(stage.get());

    auto next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(0, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(1, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(1, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(2, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(3, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(6, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT_FALSE(next.isAdvanced());

    next = densify.getNext();
    ASSERT_FALSE(next.isAdvanced());
}

TEST_F(DensifyExplicitNumericTest, CorrectlyDensifiesForNumericExplicitRangeWithDupesWithinSource) {
    auto densify = InternalDensifyStage(
        DocumentSourceInternalDensify::kStageName,
        getExpCtx(),
        "a",
        std::list<FieldPath>(),
        RangeStatement(Value(1), ExplicitBounds(Value(0), Value(20)), boost::none));
    auto stage = exec::agg::MockStage::createForTest(
        {"{a: 1}", "{a: 7}", "{a: 7}", "{a: 7}", "{a: 15}"}, getExpCtx());
    densify.setSource(stage.get());

    auto next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(0, next.getDocument().getField("a").getDouble());

    for (int i = 1; i <= 6; ++i) {
        next = densify.getNext();
        ASSERT(next.isAdvanced());
        ASSERT_EQUALS(i, next.getDocument().getField("a").getDouble());
    }

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(7, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(7, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(7, next.getDocument().getField("a").getDouble());

    for (int i = 8; i < 20; ++i) {
        next = densify.getNext();
        ASSERT(next.isAdvanced());
        ASSERT_EQUALS(i, next.getDocument().getField("a").getDouble());
    }

    next = densify.getNext();
    ASSERT_FALSE(next.isAdvanced());

    next = densify.getNext();
    ASSERT_FALSE(next.isAdvanced());
}

TEST_F(DensifyExplicitNumericTest, CorrectlyDensifiesForNumericExplicitRangeAfterHitsEOF) {
    auto densify = InternalDensifyStage(
        DocumentSourceInternalDensify::kStageName,
        getExpCtx(),
        "a",
        std::list<FieldPath>(),
        RangeStatement(Value(1), ExplicitBounds(Value(2), Value(6)), boost::none));
    auto stage = exec::agg::MockStage::createForTest({"{a: 0}"}, getExpCtx());
    densify.setSource(stage.get());

    auto next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(0, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(2, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(3, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(4, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(5, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT_FALSE(next.isAdvanced());

    next = densify.getNext();
    ASSERT_FALSE(next.isAdvanced());
}

TEST_F(DensifyExplicitNumericTest, CorrectlyDensifiesForNumericExplicitRangeWhenFieldIsMissing) {
    auto densify = InternalDensifyStage(
        DocumentSourceInternalDensify::kStageName,
        getExpCtx(),
        "a",
        std::list<FieldPath>(),
        RangeStatement(Value(1), ExplicitBounds(Value(0), Value(5)), boost::none));
    auto stage = exec::agg::MockStage::createForTest({"{b: 2}", "{b: 6}", "{a: 1}"}, getExpCtx());
    densify.setSource(stage.get());

    auto next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(2, next.getDocument().getField("b").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(6, next.getDocument().getField("b").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(0, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(1, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(2, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(3, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(4, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT_FALSE(next.isAdvanced());

    next = densify.getNext();
    ASSERT_FALSE(next.isAdvanced());
}

TEST_F(DensifyExplicitNumericTest, CorrectlyDensifiesForNumericExplicitRangeStepLargerThanRange) {
    auto densify = InternalDensifyStage(
        DocumentSourceInternalDensify::kStageName,
        getExpCtx(),
        "a",
        std::list<FieldPath>(),
        RangeStatement(Value(6), ExplicitBounds(Value(0), Value(4)), boost::none));
    auto stage = exec::agg::MockStage::createForTest({"{a: 1}", "{a: 6}"}, getExpCtx());
    densify.setSource(stage.get());

    auto next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(0, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(1, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(6, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT_FALSE(next.isAdvanced());

    next = densify.getNext();
    ASSERT_FALSE(next.isAdvanced());
}

TEST_F(DensifyExplicitNumericTest, CorrectlyDensifiesForNumericExplicitRangeHitEOFNearMax) {
    auto densify = InternalDensifyStage(
        DocumentSourceInternalDensify::kStageName,
        getExpCtx(),
        "a",
        std::list<FieldPath>(),
        RangeStatement(Value(2), ExplicitBounds(Value(0), Value(3)), boost::none));
    auto stage = exec::agg::MockStage::createForTest({"{a: 0}", "{a: 2}"}, getExpCtx());
    densify.setSource(stage.get());

    auto next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(0, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(2, next.getDocument().getField("a").getDouble());

    next = densify.getNext();
    ASSERT_FALSE(next.isAdvanced());

    next = densify.getNext();
    ASSERT_FALSE(next.isAdvanced());
}

TEST_F(DensifyExplicitNumericTest, DensificationForNumericValuesErrorsIfFieldIsNotNumeric) {
    auto densify = InternalDensifyStage(
        DocumentSourceInternalDensify::kStageName,
        getExpCtx(),
        "a",
        std::list<FieldPath>(),
        RangeStatement(Value(6), ExplicitBounds(Value(0), Value(4)), boost::none));
    auto stage =
        exec::agg::MockStage::createForTest({"{a: \"should be numeric\"}", "{a: 6}"}, getExpCtx());
    densify.setSource(stage.get());
    ASSERT_THROWS_CODE(densify.getNext(), AssertionException, 5733201);
}

TEST_F(DensifyExplicitNumericTest, DensifiesOnImmediateEOFExplictRange) {
    auto densify = InternalDensifyStage(
        DocumentSourceInternalDensify::kStageName,
        getExpCtx(),
        "a",
        std::list<FieldPath>(),
        RangeStatement(Value(1), ExplicitBounds(Value(0), Value(2)), boost::none));
    auto stage = exec::agg::MockStage::createForTest({}, getExpCtx());
    densify.setSource(stage.get());
    auto next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(0, next.getDocument().getField("a").getDouble());
    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(1, next.getDocument().getField("a").getDouble());
    next = densify.getNext();
    ASSERT_FALSE(next.isAdvanced());
}

TEST_F(DensifyFullNumericTest, DensifiesOnImmediateEOFExplicitRange) {
    auto densify = InternalDensifyStage(DocumentSourceInternalDensify::kStageName,
                                        getExpCtx(),
                                        "a",
                                        std::list<FieldPath>(),
                                        RangeStatement(Value(3), Full(), boost::none));
    auto stage = exec::agg::MockStage::createForTest({}, getExpCtx());
    densify.setSource(stage.get());
    auto next = densify.getNext();
    ASSERT_FALSE(next.isAdvanced());
}

TEST_F(DensifyPartitionNumericTest, DensifiesOnImmediateEOFExplicitRange) {
    auto densify = InternalDensifyStage(DocumentSourceInternalDensify::kStageName,
                                        getExpCtx(),
                                        "a",
                                        std::list<FieldPath>(),
                                        RangeStatement(Value(3), Full(), boost::none));
    auto stage = exec::agg::MockStage::createForTest({}, getExpCtx());
    densify.setSource(stage.get());
    auto next = densify.getNext();
    ASSERT_FALSE(next.isAdvanced());
}

TEST_F(DensifyCloneTest, InternalDensifyCanBeCloned) {

    std::list<boost::intrusive_ptr<DocumentSource>> sources;
    sources.push_back(make_intrusive<DocumentSourceInternalDensify>(
        getExpCtx(),
        "a",
        std::list<FieldPath>(),
        RangeStatement(Value(2), ExplicitBounds(Value(0), Value(3)), boost::none)));
    auto pipe = Pipeline::create(sources, getExpCtx());
    auto clonedPipe = pipe->clone();
}

TEST(DensifyStepTest, InternalDensifyIsOnStepWithNumbers) {
    auto range = RangeStatement(Value(2), Full(), boost::none);
    ASSERT_TRUE(isOnStepRelativeTo(DensifyValue(Value(5)), Value(1), range));
}

TEST(DensifyStepTest, InternalDensifyIsOffStepWithNumbers) {
    auto range = RangeStatement(Value(2), Full(), boost::none);
    ASSERT_FALSE(isOnStepRelativeTo(DensifyValue(Value(5)), Value(2), range));
}

TEST(DensifyStepTest, InternalDensifyIsOnStepWithSmallDateStep) {
    auto range = RangeStatement(Value(2), Full(), TimeUnit::second);
    auto val = DensifyValue(makeDate("2021-01-01T00:00:05.000Z"));
    auto base = DensifyValue(makeDate("2021-01-01T00:00:01.000Z"));

    ASSERT_TRUE(isOnStepRelativeTo(val, base, range));
}

TEST(DensifyStepTest, InternalDensifyIsOffStepWithSmallDateStep) {
    auto range = RangeStatement(Value(2), Full(), TimeUnit::second);
    auto val = DensifyValue(makeDate("2021-01-01T00:00:05.000Z"));
    auto base = DensifyValue(makeDate("2021-01-01T00:00:02.000Z"));

    ASSERT_FALSE(isOnStepRelativeTo(val, base, range));
}

TEST(DensifyStepTest, InternalDensifyIsOnStepWithLargeDateStep) {
    auto range = RangeStatement(Value(2), Full(), TimeUnit::month);
    auto val = DensifyValue(makeDate("2021-05-01T00:00:00.000Z"));
    auto base = DensifyValue(makeDate("2021-01-01T00:00:00.000Z"));

    ASSERT_TRUE(isOnStepRelativeTo(val, base, range));
}

TEST(DensifyStepTest, InternalDensifyIsOffStepWithLargeDateStep) {
    auto range = RangeStatement(Value(3), Full(), TimeUnit::month);
    auto val = DensifyValue(makeDate("2021-05-01T00:00:00.000Z"));
    auto base = DensifyValue(makeDate("2021-01-01T00:00:00.000Z"));

    ASSERT_FALSE(isOnStepRelativeTo(val, base, range));
}

TEST(DensifyStepTest, InternalDensifyIsOffStepForDaysWithLargeDateStep) {
    auto range = RangeStatement(Value(2), Full(), TimeUnit::month);
    auto val = DensifyValue(makeDate("2021-05-03T00:00:00.000Z"));
    auto base = DensifyValue(makeDate("2021-01-01T00:00:00.000Z"));

    ASSERT_FALSE(isOnStepRelativeTo(val, base, range));
}

TEST_F(DensifySpecTest, InvalidDensifyField) {
    auto spec = fromjson(R"({
        $densify: {
            field: "a",
            partitionByFields: ["a"],
            range: {
                step: 1,
                unit: "hour",
                bounds: [
                    {$date: "2023-04-23T00:00:00.000Z"},
                    {$date: "2023-04-23T08:00:00.000Z"}
                ]
            }
        }
    })");

    ASSERT_THROWS_CODE(
        DocumentSourceInternalDensify::createFromBson(spec.firstElement(), getExpCtx()),
        AssertionException,
        8993000);
}

TEST_F(DensifySpecTest, InvalidNestedDensifyField) {
    auto spec = fromjson(R"({
        $densify: {
            field: "a.b",
            partitionByFields: ["a"],
            range: {
                step: 1,
                unit: "hour",
                bounds: [
                    {$date: "2023-04-23T00:00:00.000Z"},
                    {$date: "2023-04-23T08:00:00.000Z"}
                ]
            }
        }
    })");

    ASSERT_THROWS_CODE(
        DocumentSourceInternalDensify::createFromBson(spec.firstElement(), getExpCtx()),
        AssertionException,
        9554500);
}

TEST_F(DensifySpecTest, InvalidNestedPartitionByFields) {
    auto spec = fromjson(R"({
        $densify: {
            field: "a",
            partitionByFields: ["a.b"],
            range: {
                step: 1,
                unit: "hour",
                bounds: [
                    {$date: "2023-04-23T00:00:00.000Z"},
                    {$date: "2023-04-23T08:00:00.000Z"}
                ]
            }
        }
    })");

    ASSERT_THROWS_CODE(
        DocumentSourceInternalDensify::createFromBson(spec.firstElement(), getExpCtx()),
        AssertionException,
        8993000);
}

TEST_F(DensifyRedactionTest, RedactionDateBounds) {
    auto spec = fromjson(R"({
        $densify: {
            field: "a",
            range: {
                step: 1,
                unit: "hour",
                bounds: [
                    {$date: "2023-04-23T00:00:00.000Z"},
                    {$date: "2023-04-23T08:00:00.000Z"}
                ]
            }
        }
    })");

    auto docSource =
        DocumentSourceInternalDensify::createFromBson(spec.firstElement(), getExpCtx());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$_internalDensify": {
                "field": "HASH<a>",
                "partitionByFields": [],
                "range": {
                    "step": "?number",
                    "bounds": [
                        "?date",
                        "?date"
                    ],
                    "unit": "hour"
                }
            }
        })",
        redact(*docSource));
}

TEST_F(DensifyRedactionTest, RedactionFullBoundsWithPartitionFields) {
    auto spec = fromjson(R"({
        $densify: {
            field: "foo",
            partitionByFields: ["a", "b", "c.d"],
            range: {
                bounds: "full",
                step: 100
            }
        }
    })");
    auto docSource =
        DocumentSourceInternalDensify::createFromBson(spec.firstElement(), getExpCtx());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$_internalDensify": {
                "field": "HASH<foo>",
                "partitionByFields": [
                    "HASH<a>",
                    "HASH<b>",
                    "HASH<c>.HASH<d>"
                ],
                "range": {
                    "step": "?number",
                    "bounds": "full"
                }
            }
        })",
        redact(*docSource));
}

TEST_F(DensifyRedactionTest, RedactionPartitionBounds) {
    auto spec = fromjson(R"({
        $densify: {
            field: "x",
            partitionByFields: ["foo"],
            range: {
                bounds: "partition",
                step: 50,
                unit: "second"
            }
        }
    })");
    auto docSource =
        DocumentSourceInternalDensify::createFromBson(spec.firstElement(), getExpCtx());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$_internalDensify": {
                "field": "HASH<x>",
                "partitionByFields": [
                    "HASH<foo>"
                ],
                "range": {
                    "step": "?number",
                    "bounds": "partition",
                    "unit": "second"
                }
            }
        })",
        redact(*docSource));
}

void assertRangeTimeUnitSerialization(auto expCtx,
                                      BSONObj inputStage,
                                      auto expectedStage,
                                      auto opts) {
    auto parsedStage =
        DocumentSourceInternalDensify::createFromBson(inputStage.firstElement(), expCtx);
    std::vector<Value> serialization;
    parsedStage->serializeToArray(serialization, opts);

    auto serializedStage = serialization[0].getDocument().toBson();
    ASSERT_BSONOBJ_EQ(expectedStage, serializedStage);
}

TEST_F(DensifyRedactionTest, RangeTimeUnitSerializationRepresentative) {
    assertRangeTimeUnitSerialization(
        getExpCtx(),
        fromjson(R"({
            $densify: {
                field: "x",
                partitionByFields: ["foo"],
                range: {
                    bounds: "partition",
                    step: 50,
                    unit: "second"
                }
            }
        })"),
        fromjson(R"({
            $_internalDensify: {
                field: "x",
                partitionByFields: [
                    "foo"
                ],
                range: {
                    step: 1,
                    bounds: "partition",
                    unit: "second"
                }
            }
        })"),
        SerializationOptions::kRepresentativeQueryShapeSerializeOptions);
}

TEST_F(DensifyRedactionTest, RangeTimeUnitSerializationDebug) {
    assertRangeTimeUnitSerialization(getExpCtx(),
                                     fromjson(
                                         R"({
            $densify: {
                field: "x",
                partitionByFields: ["foo"],
                range: {
                    bounds: "partition",
                    step: 50,
                    unit: "second"
                }
            }
        })"),
                                     fromjson(
                                         R"({
            $_internalDensify: {
                field: "x",
                partitionByFields: ["foo"],
                range: {
                    step: "?number",
                    bounds: "partition",
                    unit: "second"
                }
            }
        })"),
                                     SerializationOptions::kDebugQueryShapeSerializeOptions);
}

TEST_F(DensifyMonthStepTest, CorrectlyDensifiesForDateExplicitRangeStartingInsideMonthStep) {
    // Starting at January 31, with a step of 9 Months, densify steps back initially to April 30th
    // before starting with generation. This test ensures that when the step is added back we don't
    // generate January 30th which is outside of the range.
    auto densify =
        InternalDensifyStage(DocumentSourceInternalDensify::kStageName,
                             getExpCtx(),
                             "a",
                             std::list<FieldPath>(),
                             RangeStatement(Value(9),
                                            ExplicitBounds(makeDate("2019-01-31T21:53:45.323Z"),
                                                           makeDate("2020-06-02T10:40:15.842Z")),
                                            TimeUnit::month));

    Document doc{{"a", makeDate("2019-10-01T00:00:00.000Z")}};
    auto stage = exec::agg::MockStage::createForTest(doc, getExpCtx());
    densify.setSource(stage.get());

    auto next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(makeDate("2019-01-31T21:53:45.323Z"), next.getDocument().getField("a").getDate());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(makeDate("2019-10-01T00:00:00.000Z"), next.getDocument().getField("a").getDate());

    next = densify.getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(makeDate("2019-10-31T21:53:45.323Z"), next.getDocument().getField("a").getDate());

    next = densify.getNext();
    ASSERT_FALSE(next.isAdvanced());

    next = densify.getNext();
    ASSERT_FALSE(next.isAdvanced());
}
}  // namespace
}  // namespace mongo
