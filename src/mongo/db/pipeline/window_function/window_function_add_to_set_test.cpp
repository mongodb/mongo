// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/window_function/window_function_add_to_set.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <string>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

class WindowFunctionAddToSetTest : public AggregationContextFixture {
public:
    WindowFunctionAddToSetTest() : expCtx(getExpCtx()), addToSet(expCtx.get()) {}

    void addValuesToWindow(const std::vector<Value>& values) {
        for (const auto& val : values)
            addToSet.add(val);
    }

    boost::intrusive_ptr<ExpressionContext> expCtx;
    WindowFunctionAddToSet addToSet;
};

TEST_F(WindowFunctionAddToSetTest, EmptyWindowReturnsDefault) {
    ASSERT_VALUE_EQ(addToSet.getValue(), Value{std::vector<Value>()});

    addToSet.add(Value{1});
    addToSet.remove(Value{1});
    ASSERT_VALUE_EQ(addToSet.getValue(), Value{std::vector<Value>()});
}

TEST_F(WindowFunctionAddToSetTest, SingletonWindowShouldReturnAVector) {
    addToSet.add(Value{5});
    ASSERT_VALUE_EQ(addToSet.getValue(), Value{std::vector<Value>{Value{5}}});

    addToSet.reset();
    addToSet.add(Value{std::string("str")});
    ASSERT_VALUE_EQ(addToSet.getValue(), Value{std::vector<Value>{Value{std::string("str")}}});
}

TEST_F(WindowFunctionAddToSetTest, ComplexWindow) {
    addValuesToWindow({Value{2}, Value{5}, Value{std::string("str")}, Value{BSONObj()}});

    std::vector<Value> expected = {Value{2}, Value{5}, Value{std::string("str")}, Value{BSONObj()}};
    ASSERT_VALUE_EQ(addToSet.getValue(), Value{expected});
}

TEST_F(WindowFunctionAddToSetTest, Removal) {
    addValuesToWindow({Value{1}, Value{2}});
    addToSet.remove(Value{1});

    std::vector<Value> expected = {Value{2}};
    ASSERT_VALUE_EQ(addToSet.getValue(), Value{expected});
}

TEST_F(WindowFunctionAddToSetTest, NotAllowDuplicates) {
    addValuesToWindow({Value{1}, Value{1}, Value{2}, Value{3}});

    // $addToSet window function returns a vector of values that are in a set excluding duplicates.
    std::vector<Value> expected = {Value{1}, Value{2}, Value{3}};
    ASSERT_VALUE_EQ(addToSet.getValue(), Value{expected});

    addToSet.remove(Value{1});
    // Have to remove element '1' twice to remove it from the set.
    ASSERT_VALUE_EQ(addToSet.getValue(), Value{expected});

    addToSet.remove(Value{1});
    expected = {Value{2}, Value{3}};
    ASSERT_VALUE_EQ(addToSet.getValue(), Value{expected});
}

TEST_F(WindowFunctionAddToSetTest, NestedArraysShouldReturnNestedArrays) {
    Value vecValue{std::vector<Value>{{Value{1}, Value{2}, Value{3}}}};
    addValuesToWindow({vecValue, vecValue, vecValue});

    std::vector<Value> expected = {vecValue};
    ASSERT_VALUE_EQ(addToSet.getValue(), Value{expected});
}

TEST_F(WindowFunctionAddToSetTest, IdenticalDocsAndDocsWithDifferentFieldsOrder) {
    Value doc1{Document({{"a", 1}, {"b", 2}, {"c", 3}})};
    Value doc2{Document({{"a", 1}, {"c", 3}, {"b", 2}})};
    Value doc3{Document({{"a", 1}, {"b", 2}, {"c", 3}})};
    Value doc4{Document({{"a", 3}, {"b", 3}, {"c", 3}})};

    addValuesToWindow({doc1, doc2, doc3, doc4});

    std::vector<Value> expected = {doc1, doc2, doc4};  // doc1 and doc3 are identical.
    ASSERT_VALUE_EQ(addToSet.getValue(), Value{expected});

    addToSet.remove(doc1);
    ASSERT_VALUE_EQ(addToSet.getValue(), Value{expected});

    addToSet.remove(doc2);
    expected = {doc1, doc4};
    ASSERT_VALUE_EQ(addToSet.getValue(), Value{expected});
}

}  // namespace
}  // namespace mongo
