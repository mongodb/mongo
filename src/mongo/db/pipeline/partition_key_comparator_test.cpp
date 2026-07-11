// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/partition_key_comparator.h"

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

using PartitionKeyComparatorTestDeathTest = PartitionKeyComparatorTest;
DEATH_TEST_F(PartitionKeyComparatorTestDeathTest,
             FailsWithNullExpressionContext,
             "Null expression context") {
    boost::intrusive_ptr<ExpressionFieldPath> expr =
        ExpressionFieldPath::parse(getExpCtx().get(), "$a", getExpCtx()->variablesParseState);
    _keyComparator = std::make_unique<PartitionKeyComparator>(nullptr, expr, Document{{}});
}

DEATH_TEST_F(PartitionKeyComparatorTestDeathTest,
             FailsWithNullExpression,
             "Null expression passed") {
    _keyComparator =
        std::make_unique<PartitionKeyComparator>(getExpCtx().get(), nullptr, Document{{}});
}
}  // namespace

}  // namespace mongo
