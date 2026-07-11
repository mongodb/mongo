// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/window_function/window_function_push.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/unittest/unittest.h"

#include <string>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

class WindowFunctionPushTest : public AggregationContextFixture {
public:
    WindowFunctionPushTest() : expCtx(getExpCtx()), push(expCtx.get()) {}

    void addValuesToWindow(const std::vector<Value>& values) {
        for (const auto& val : values)
            push.add(val);
    }

    boost::intrusive_ptr<ExpressionContext> expCtx;
    WindowFunctionPush push;
};

TEST_F(WindowFunctionPushTest, EmptyWindowReturnsDefault) {
    ASSERT_VALUE_EQ(push.getValue(), Value{std::vector<Value>()});

    push.add(Value{1});
    push.remove(Value{1});
    ASSERT_VALUE_EQ(push.getValue(), Value{std::vector<Value>()});
}

TEST_F(WindowFunctionPushTest, SingletonWindowShouldReturnAVector) {
    push.add(Value{5});
    ASSERT_VALUE_EQ(push.getValue(), Value(std::vector<Value>{Value{5}}));

    push.reset();
    push.add(Value{std::string("str")});
    ASSERT_VALUE_EQ(push.getValue(), Value(std::vector<Value>{Value{std::string("str")}}));
}

TEST_F(WindowFunctionPushTest, ComplexWindowReservesInsertionOrder) {
    addValuesToWindow({Value{std::string("str")}, Value{5}, Value{2}, Value{BSONObj()}});

    std::vector<Value> expected = {Value{std::string("str")}, Value{5}, Value{2}, Value{BSONObj()}};
    ASSERT_VALUE_EQ(push.getValue(), Value{expected});
}

TEST_F(WindowFunctionPushTest, Removal) {
    addValuesToWindow({Value{1}, Value{2}});
    push.remove(Value{1});

    std::vector<Value> expected = {Value{2}};
    ASSERT_VALUE_EQ(push.getValue(), Value{expected});
}

TEST_F(WindowFunctionPushTest, AllowsDuplicates) {
    addValuesToWindow({Value{1}, Value{1}, Value{2}, Value{3}});

    // $push allows duplicates in the array returned by the window function.
    std::vector<Value> expected = {Value{1}, Value{1}, Value{2}, Value{3}};
    ASSERT_VALUE_EQ(push.getValue(), Value{expected});

    // remove() value '1' once only removes one instance in the array.
    push.remove(Value{1});
    expected = {Value{1}, Value{2}, Value{3}};

    push.remove(Value{1});
    expected = {Value{2}, Value{3}};
    ASSERT_VALUE_EQ(push.getValue(), Value{expected});
}

TEST_F(WindowFunctionPushTest, ShouldReserveInsertionOrder) {
    addValuesToWindow({Value{1234}, Value{123}, Value{321}, Value{10000}});

    std::vector<Value> expected = {Value{1234}, Value{123}, Value{321}, Value{10000}};
    ASSERT_VALUE_EQ(push.getValue(), Value{expected});
}

TEST_F(WindowFunctionPushTest, RemovalDoesNotAffectOrder) {
    addValuesToWindow({Value{1234}, Value{123}, Value{321}, Value{10000}});

    std::vector<Value> expected = {Value{1234}, Value{123}, Value{321}, Value{10000}};
    ASSERT_VALUE_EQ(push.getValue(), Value{expected});

    push.remove(Value{1234});
    expected = {Value{123}, Value{321}, Value{10000}};
    ASSERT_VALUE_EQ(push.getValue(), Value{expected});
}

TEST_F(WindowFunctionPushTest, NestedArraysShouldReturnNestedArrays) {
    Value vecValue{std::vector<Value>{{Value{1}, Value{2}, Value{3}}}};
    addValuesToWindow({vecValue, vecValue, vecValue});

    std::vector<Value> expected = {vecValue, vecValue, vecValue};
    ASSERT_VALUE_EQ(push.getValue(), Value{expected});

    push.remove(vecValue);
    push.remove(vecValue);
    expected = {vecValue};
    ASSERT_VALUE_EQ(push.getValue(), Value{expected});
}

TEST_F(WindowFunctionPushTest, IdenticalDocsAndDocsWithDifferentFieldsOrder) {
    Value doc1{Document({{"a", 1}, {"b", 2}, {"c", 3}})};
    Value doc2{Document({{"a", 1}, {"b", 2}, {"c", 3}})};
    Value doc3{Document({{"a", 1}, {"c", 3}, {"b", 2}})};
    Value doc4{Document({{"a", 3}, {"b", 3}, {"c", 3}})};

    addValuesToWindow({doc1, doc2, doc3, doc4});

    std::vector<Value> expected = {doc1, doc2, doc3, doc4};
    ASSERT_VALUE_EQ(push.getValue(), Value{expected});

    push.remove(doc1);
    expected = {doc2, doc3, doc4};
    ASSERT_VALUE_EQ(push.getValue(), Value{expected});

    push.remove(doc2);
    expected = {doc3, doc4};
    ASSERT_VALUE_EQ(push.getValue(), Value{expected});
}

}  // namespace
}  // namespace mongo
