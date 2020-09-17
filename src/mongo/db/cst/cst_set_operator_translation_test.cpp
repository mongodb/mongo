/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <boost/intrusive_ptr.hpp>
#include <string>

#include "mongo/db/cst/c_node.h"
#include "mongo/db/cst/cst_pipeline_translation.h"
#include "mongo/db/cst/key_fieldname.h"
#include "mongo/db/cst/key_value.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

auto getExpCtx() {
    auto nss = NamespaceString{"db", "coll"};
    return ExpressionContextForTest(nss);
}

TEST(CstPipelineTranslationTest, AllElementsTrueTest) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::allElementsTrue,
         CNode{CNode::ArrayChildren{CNode{AggregationPath{makeVector<std::string>("set"s)}}}}}}};
    auto expCtx = getExpCtx();
    auto expr = cst_pipeline_translation::translateExpression(cst, &expCtx);
    ASSERT_TRUE(ValueComparator().evaluate(Value(fromjson("{$allElementsTrue: [\"$set\"]}")) ==
                                           expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, AnyElementsTrueTest) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::anyElementTrue,
         CNode{CNode::ArrayChildren{CNode{AggregationPath{makeVector<std::string>("set"s)}}}}}}};
    auto expCtx = getExpCtx();
    auto expr = cst_pipeline_translation::translateExpression(cst, &expCtx);
    ASSERT_TRUE(ValueComparator().evaluate(Value(fromjson("{$anyElementTrue: [\"$set\"]}")) ==
                                           expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, SetDifferenceTest) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::setDifference,
         CNode{CNode::ArrayChildren{CNode{AggregationPath{makeVector<std::string>("set"s)}},
                                    CNode{AggregationPath{makeVector<std::string>("set2"s)}}}}}}};
    auto expCtx = getExpCtx();
    auto expr = cst_pipeline_translation::translateExpression(cst, &expCtx);
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$setDifference: [\"$set\", \"$set2\"]}")) == expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, SetEqualsTest) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::setEquals,
         CNode{CNode::ArrayChildren{CNode{AggregationPath{makeVector<std::string>("set"s)}},
                                    CNode{AggregationPath{makeVector<std::string>("set2"s)}}}}}}};
    auto expCtx = getExpCtx();
    auto expr = cst_pipeline_translation::translateExpression(cst, &expCtx);
    ASSERT_TRUE(ValueComparator().evaluate(Value(fromjson("{$setEquals: [\"$set\", \"$set2\"]}")) ==
                                           expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, SetIntersectionTest) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::setIntersection,
         CNode{CNode::ArrayChildren{CNode{AggregationPath{makeVector<std::string>("set"s)}},
                                    CNode{AggregationPath{makeVector<std::string>("set2"s)}},
                                    CNode{AggregationPath{makeVector<std::string>("set3"s)}}}}}}};
    auto expCtx = getExpCtx();
    auto expr = cst_pipeline_translation::translateExpression(cst, &expCtx);
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$setIntersection: [\"$set\", \"$set2\", \"$set3\"]}")) ==
        expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, SetIsSubsetTest) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::setIsSubset,
         CNode{CNode::ArrayChildren{CNode{AggregationPath{makeVector<std::string>("set"s)}},
                                    CNode{AggregationPath{makeVector<std::string>("set2"s)}}}}}}};
    auto expCtx = getExpCtx();
    auto expr = cst_pipeline_translation::translateExpression(cst, &expCtx);
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$setIsSubset: [\"$set\", \"$set2\"]}")) == expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, SetUnionTest) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::setUnion,
         CNode{CNode::ArrayChildren{CNode{AggregationPath{makeVector<std::string>("set"s)}},
                                    CNode{AggregationPath{makeVector<std::string>("set2"s)}},
                                    CNode{AggregationPath{makeVector<std::string>("set3"s)}}}}}}};
    auto expCtx = getExpCtx();
    auto expr = cst_pipeline_translation::translateExpression(cst, &expCtx);
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$setUnion: [\"$set\", \"$set2\", \"$set3\"]}")) ==
        expr->serialize(false)));
}

}  // namespace
}  // namespace mongo
