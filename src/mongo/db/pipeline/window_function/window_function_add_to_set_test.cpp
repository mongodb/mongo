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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/window_function/window_function_add_to_set.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class WindowFunctionAddToSetTest : public AggregationContextFixture {
public:
    WindowFunctionAddToSetTest() : expCtx(getExpCtx()), addToSet(expCtx.get()) {}

    void addValuesToWindow(const std::vector<Value>& values) {
        for (auto val : values)
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
