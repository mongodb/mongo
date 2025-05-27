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

#include "mongo/db/pipeline/partition_key_comparator.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

class PartitionKeyComparatorTest : public AggregationContextFixture {
public:
    void makeWithDefaultExpression(Document doc) {
        boost::intrusive_ptr<ExpressionFieldPath> expr =
            ExpressionFieldPath::parse(getExpCtx().get(), "$a", getExpCtx()->variablesParseState);
        _keyComparator = std::make_unique<PartitionKeyComparator>(getExpCtx().get(), expr, doc);
    }

protected:
    std::unique_ptr<PartitionKeyComparator> _keyComparator = nullptr;
};

TEST_F(PartitionKeyComparatorTest, DetectsPartitionChangeBetweenDocuments) {
    Document docOne{{"a", 1}};
    makeWithDefaultExpression(docOne);
    Document docTwo{{"a", 2}};
    ASSERT_TRUE(_keyComparator->isDocumentNewPartition(docTwo));
}

TEST_F(PartitionKeyComparatorTest, DetectsPartitionChangeAfterMultipleDocuments) {
    Document docOne{{"a", 1}, {"b", 1}};
    makeWithDefaultExpression(docOne);
    Document docTwo{{"a", 1}, {"b", 2}};
    Document docThree{{"a", 1}, {"b", 3}};
    Document docFour{{"a", 2}, {"b", 1}};
    ASSERT_FALSE(_keyComparator->isDocumentNewPartition(docTwo));
    ASSERT_FALSE(_keyComparator->isDocumentNewPartition(docThree));
    ASSERT_TRUE(_keyComparator->isDocumentNewPartition(docFour));
}

TEST_F(PartitionKeyComparatorTest, ReportsMemoryCorrectly) {
    Document docOne{{"a", 1}, {"b", 1}};
    makeWithDefaultExpression(docOne);
    ASSERT_EQ(_keyComparator->getApproximateSize(), docOne["a"].getApproximateSize());
    Document docTwo{{"a", std::string("seventeen")}, {"b", 2}};
    ASSERT_TRUE(_keyComparator->isDocumentNewPartition(docTwo));
    ASSERT_EQ(_keyComparator->getApproximateSize(), docTwo["a"].getApproximateSize());
}

TEST_F(PartitionKeyComparatorTest, NullAndMissingCompareEqual) {
    Document docOne{{"a", BSONNULL}};
    makeWithDefaultExpression(docOne);
    Document docTwo{{"b", 5}};
    Document docThree{{"a", BSONNULL}};
    Document docFour{{"a", 3}};
    ASSERT_FALSE(_keyComparator->isDocumentNewPartition(docTwo));
    ASSERT_FALSE(_keyComparator->isDocumentNewPartition(docThree));
    ASSERT_TRUE(_keyComparator->isDocumentNewPartition(docFour));
}

DEATH_TEST_F(PartitionKeyComparatorTest,
             FailsWithNullExpressionContext,
             "Null expression context") {
    boost::intrusive_ptr<ExpressionFieldPath> expr =
        ExpressionFieldPath::parse(getExpCtx().get(), "$a", getExpCtx()->variablesParseState);
    ASSERT_THROWS_CODE(_keyComparator =
                           std::make_unique<PartitionKeyComparator>(nullptr, expr, Document{{}}),
                       AssertionException,
                       5733800);
}

DEATH_TEST_F(PartitionKeyComparatorTest, FailsWithNullExpression, "Null expression passed") {
    ASSERT_THROWS_CODE(_keyComparator = std::make_unique<PartitionKeyComparator>(
                           getExpCtx().get(), nullptr, Document{{}}),
                       AssertionException,
                       5733801);
}
}  // namespace

}  // namespace mongo
