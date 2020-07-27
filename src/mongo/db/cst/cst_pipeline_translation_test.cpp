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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/cst/c_node.h"
#include "mongo/db/cst/cst_pipeline_translation.h"
#include "mongo/db/cst/key_fieldname.h"
#include "mongo/db/cst/key_value.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
using namespace std::string_literals;

auto getExpCtx() {
    auto nss = NamespaceString{"db", "coll"};
    return boost::intrusive_ptr<ExpressionContextForTest>{new ExpressionContextForTest(nss)};
}

TEST(CstPipelineTranslationTest, TranslatesEmpty) {
    const auto cst = CNode{CNode::ArrayChildren{}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(0u, sources.size());
}

TEST(CstPipelineTranslationTest, TranslatesEmptyProject) {
    const auto cst = CNode{CNode::ArrayChildren{
        CNode{CNode::ObjectChildren{{KeyFieldname::project, CNode{CNode::ObjectChildren{}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    ASSERT(typeid(DocumentSourceSingleDocumentTransformation) == typeid(**iter));
}

TEST(CstPipelineTranslationTest, TranslatesEmptyProjects) {
    const auto cst = CNode{CNode::ArrayChildren{
        CNode{CNode::ObjectChildren{{KeyFieldname::project, CNode{CNode::ObjectChildren{}}}}},
        CNode{CNode::ObjectChildren{{KeyFieldname::project, CNode{CNode::ObjectChildren{}}}}},
        CNode{CNode::ObjectChildren{{KeyFieldname::project, CNode{CNode::ObjectChildren{}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(3u, sources.size());
    auto iter = sources.begin();
    ASSERT(typeid(DocumentSourceSingleDocumentTransformation) == typeid(**iter++));
    ASSERT(typeid(DocumentSourceSingleDocumentTransformation) == typeid(**iter++));
    ASSERT(typeid(DocumentSourceSingleDocumentTransformation) == typeid(**iter));
}

TEST(CstPipelineTranslationTest, TranslatesOneFieldInclusionProjectionStage) {
    const auto cst = CNode{CNode::ArrayChildren{CNode{CNode::ObjectChildren{
        {KeyFieldname::project,
         CNode{CNode::ObjectChildren{{UserFieldname{"a"}, CNode{KeyValue::trueKey}}}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    auto& singleDoc = dynamic_cast<DocumentSourceSingleDocumentTransformation&>(**iter);
    // DocumenSourceSingleDoucmentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << true << "a" << true) ==
        singleDoc.getTransformer().serializeTransformation(boost::none).toBson()));
}

TEST(CstPipelineTranslationTest, TranslatesMultifieldInclusionProjection) {
    const auto cst = CNode{CNode::ArrayChildren{CNode{CNode::ObjectChildren{
        {KeyFieldname::project,
         CNode{CNode::ObjectChildren{{KeyFieldname::id, CNode{KeyValue::trueKey}},
                                     {UserFieldname{"a"}, CNode{NonZeroKey{7}}},
                                     {UserFieldname{"b"}, CNode{NonZeroKey{-99999999999ll}}}}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    auto& singleDoc = dynamic_cast<DocumentSourceSingleDocumentTransformation&>(**iter);
    // DocumenSourceSingleDoucmentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << true << "a" << true << "b" << true) ==
        singleDoc.getTransformer().serializeTransformation(boost::none).toBson()));
}

TEST(CstPipelineTranslationTest, TranslatesOneFieldExclusionProjectionStage) {
    const auto cst = CNode{CNode::ArrayChildren{CNode{CNode::ObjectChildren{
        {KeyFieldname::project,
         CNode{CNode::ObjectChildren{{UserFieldname{"a"}, CNode{KeyValue::falseKey}}}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    auto& singleDoc = dynamic_cast<DocumentSourceSingleDocumentTransformation&>(**iter);
    // DocumenSourceSingleDoucmentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("a" << false) ==
        singleDoc.getTransformer().serializeTransformation(boost::none).toBson()));
}

TEST(CstPipelineTranslationTest, TranslatesMultifieldExclusionProjection) {
    const auto cst = CNode{CNode::ArrayChildren{CNode{CNode::ObjectChildren{
        {KeyFieldname::project,
         CNode{CNode::ObjectChildren{{KeyFieldname::id, CNode{KeyValue::falseKey}},
                                     {UserFieldname{"a"}, CNode{KeyValue::doubleZeroKey}},
                                     {UserFieldname{"b"}, CNode{KeyValue::decimalZeroKey}}}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    auto& singleDoc = dynamic_cast<DocumentSourceSingleDocumentTransformation&>(**iter);
    // DocumenSourceSingleDoucmentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << false << "a" << false << "b" << false) ==
        singleDoc.getTransformer().serializeTransformation(boost::none).toBson()));
}

TEST(CstPipelineTranslationTest, FailsToTranslateInclusionExclusionMixedProjectionStage) {
    const auto cst = CNode{CNode::ArrayChildren{CNode{CNode::ObjectChildren{
        {KeyFieldname::project,
         CNode{CNode::ObjectChildren{{UserFieldname{"a"}, CNode{KeyValue::trueKey}},
                                     {UserFieldname{"b"}, CNode{KeyValue::falseKey}}}}}}}}};
    ASSERT_THROWS_CODE(
        cst_pipeline_translation::translatePipeline(cst, getExpCtx()), DBException, 4933100);
}

TEST(CstPipelineTranslationTest, TranslatesComputedProjection) {
    const auto cst = CNode{CNode::ArrayChildren{CNode{CNode::ObjectChildren{
        {KeyFieldname::project,
         CNode{CNode::ObjectChildren{
             {UserFieldname{"a"},
              CNode{CNode::ObjectChildren{
                  {KeyFieldname::atan2,
                   CNode{CNode::ArrayChildren{CNode{UserInt{1}}, CNode{UserInt{0}}}}}}}},
             {UserFieldname{"b"},
              CNode{
                  CNode::ObjectChildren{{KeyFieldname::add,
                                         CNode{CNode::ArrayChildren{CNode{UserInt{1}},
                                                                    CNode{UserInt{2}},
                                                                    CNode{UserInt{3}},
                                                                    CNode{UserInt{4}}}}}}}}}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    auto& singleDoc = dynamic_cast<DocumentSourceSingleDocumentTransformation&>(**iter);
    // DocumenSourceSingleDoucmentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << true << "a"
                   << BSON("$atan2" << BSON_ARRAY(BSON("$const" << 1) << BSON("$const" << 0)))
                   << "b"
                   << BSON("$add" << BSON_ARRAY(BSON("$const" << 1)
                                                << BSON("$const" << 2) << BSON("$const" << 3)
                                                << BSON("$const" << 4)))) ==
        singleDoc.getTransformer().serializeTransformation(boost::none).toBson()));
}

TEST(CstPipelineTranslationTest, FailsToTranslateComputedExclusionMixedProjectionStage) {
    const auto cst = CNode{CNode::ArrayChildren{CNode{CNode::ObjectChildren{
        {KeyFieldname::project,
         CNode{CNode::ObjectChildren{
             {UserFieldname{"a"},
              CNode{CNode::ObjectChildren{
                  {KeyFieldname::atan2,
                   CNode{CNode::ArrayChildren{CNode{UserDouble{1.0}}, CNode{UserDecimal{0.0}}}}}}}},
             {UserFieldname{"b"}, CNode{KeyValue::falseKey}}}}}}}}};
    ASSERT_THROWS_CODE(
        cst_pipeline_translation::translatePipeline(cst, getExpCtx()), DBException, 4933100);
}

TEST(CstPipelineTranslationTest, TranslatesComputedInclusionMixedProjectionStage) {
    const auto cst = CNode{CNode::ArrayChildren{CNode{CNode::ObjectChildren{
        {KeyFieldname::project,
         CNode{CNode::ObjectChildren{
             {UserFieldname{"a"},
              CNode{CNode::ObjectChildren{
                  {KeyFieldname::add,
                   CNode{CNode::ArrayChildren{CNode{UserLong{0ll}}, CNode{UserInt{1}}}}}}}},
             {UserFieldname{"b"}, CNode{NonZeroKey{Decimal128{590.095}}}}}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    auto& singleDoc = dynamic_cast<DocumentSourceSingleDocumentTransformation&>(**iter);
    // DocumenSourceSingleDoucmentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << true << "a"
                   << BSON("$add" << BSON_ARRAY(BSON("$const" << 0ll) << BSON("$const" << 1)))
                   << "b" << true) ==
        singleDoc.getTransformer().serializeTransformation(boost::none).toBson()));
}

TEST(CstPipelineTranslationTest, TranslatesMultipleProjectionStages) {
    // [
    //     { $project: { a: true },
    //     { $project: { b: false },
    //     { $project: { c: { $add: [
    //         { $const: 2.2 },
    //         { $atan2: [ { $const: 1 }, { $const: 0 } ] },
    //         { $const: 3 } ] } } }
    // ]
    const auto cst = CNode{CNode::ArrayChildren{
        CNode{CNode::ObjectChildren{
            {KeyFieldname::project,
             CNode{CNode::ObjectChildren{{UserFieldname{"a"}, CNode{KeyValue::trueKey}}}}}}},
        CNode{CNode::ObjectChildren{
            {KeyFieldname::project,
             CNode{CNode::ObjectChildren{{UserFieldname{"b"}, CNode{KeyValue::falseKey}}}}}}},
        CNode{
            CNode::ObjectChildren{{KeyFieldname::project,
                                   CNode{CNode::ObjectChildren{
                                       {UserFieldname{"c"},
                                        CNode{CNode::ObjectChildren{
                                            {KeyFieldname::add,
                                             CNode{CNode::ArrayChildren{
                                                 CNode{UserDouble{2.2}},
                                                 CNode{CNode::ObjectChildren{
                                                     {KeyFieldname::atan2,
                                                      CNode{CNode::ArrayChildren{
                                                          CNode{UserInt{1}}, CNode{UserInt{0}}}}}}},
                                                 CNode{UserLong{3ll}}}}}}}}}}}}},
    }};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(3u, sources.size());
    auto iter = sources.begin();
    {
        auto& singleDoc = dynamic_cast<DocumentSourceSingleDocumentTransformation&>(**iter++);
        // DocumenSourceSingleDoucmentTransformation reorders fields so we need to be
        // insensitive.
        ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
            BSON("_id" << true << "a" << true) ==
            singleDoc.getTransformer().serializeTransformation(boost::none).toBson()));
    }
    {
        auto& singleDoc = dynamic_cast<DocumentSourceSingleDocumentTransformation&>(**iter++);
        // DocumenSourceSingleDoucmentTransformation reorders fields so we need to be
        // insensitive.
        ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
            BSON("b" << false) ==
            singleDoc.getTransformer().serializeTransformation(boost::none).toBson()));
    }
    {
        auto& singleDoc = dynamic_cast<DocumentSourceSingleDocumentTransformation&>(**iter);
        // DocumenSourceSingleDoucmentTransformation reorders fields so we need to be
        // insensitive.
        ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
            BSON("_id" << true << "c"
                       << BSON("$add"
                               << BSON_ARRAY(BSON("$const" << 2.2)
                                             << BSON("$atan2" << BSON_ARRAY(BSON("$const" << 1)
                                                                            << BSON("$const" << 0)))
                                             << BSON("$const" << 3ll)))) ==
            singleDoc.getTransformer().serializeTransformation(boost::none).toBson()));
    }
}

TEST(CstPipelineTranslationTest, TranslatesMultipleProjectionStagesWithAndOrNot) {
    // [
    //     { $project: { a: { $not: [
    //         { $const: 0 } },
    //     { $project: { c: { $and: [
    //         { $const: 2.2 },
    //         { $or: [ { $const: 1 }, { $const: 0 } ] },
    //         { $const: 3 } ] } } }
    // ]
    const auto cst = CNode{CNode::ArrayChildren{
        CNode{CNode::ObjectChildren{
            {KeyFieldname::project,
             CNode{CNode::ObjectChildren{
                 {UserFieldname{"a"},
                  CNode{CNode::ObjectChildren{
                      {KeyFieldname::notExpr,
                       CNode{CNode::ArrayChildren{CNode{UserInt{0}}}}}}}}}}}}},
        CNode{
            CNode::ObjectChildren{{KeyFieldname::project,
                                   CNode{CNode::ObjectChildren{
                                       {UserFieldname{"c"},
                                        CNode{CNode::ObjectChildren{
                                            {KeyFieldname::andExpr,
                                             CNode{CNode::ArrayChildren{
                                                 CNode{UserDouble{2.2}},
                                                 CNode{CNode::ObjectChildren{
                                                     {KeyFieldname::orExpr,
                                                      CNode{CNode::ArrayChildren{
                                                          CNode{UserInt{1}}, CNode{UserInt{0}}}}}}},
                                                 CNode{UserLong{3ll}}}}}}}}}}}}},
    }};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(2u, sources.size());
    auto iter = sources.begin();
    {
        auto& singleDoc = dynamic_cast<DocumentSourceSingleDocumentTransformation&>(**iter++);
        // DocumenSourceSingleDoucmentTransformation reorders fields so we need to be
        // insensitive.
        ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
            BSON("_id" << true << "a" << BSON("$not" << BSON_ARRAY(BSON("$const" << 0)))) ==
            singleDoc.getTransformer().serializeTransformation(boost::none).toBson()));
    }
    {
        auto& singleDoc = dynamic_cast<DocumentSourceSingleDocumentTransformation&>(**iter);
        // DocumenSourceSingleDoucmentTransformation reorders fields so we need to be
        // insensitive.
        ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
            BSON("_id" << true << "c"
                       << BSON("$and"
                               << BSON_ARRAY(BSON("$const" << 2.2)
                                             << BSON("$or" << BSON_ARRAY(BSON("$const" << 1)
                                                                         << BSON("$const" << 0)))
                                             << BSON("$const" << 3ll)))) ==
            singleDoc.getTransformer().serializeTransformation(boost::none).toBson()));
    }
}

TEST(CstPipelineTranslationTest, TranslatesComputedProjectionWithAndOr) {
    const auto cst = CNode{CNode::ArrayChildren{CNode{CNode::ObjectChildren{
        {KeyFieldname::project,
         CNode{CNode::ObjectChildren{
             {UserFieldname{"a"},
              CNode{CNode::ObjectChildren{
                  {KeyFieldname::andExpr,
                   CNode{CNode::ArrayChildren{
                       CNode{UserInt{1}},
                       CNode{CNode::ObjectChildren{
                           {KeyFieldname::add,
                            CNode{CNode::ArrayChildren{CNode{UserInt{1}},
                                                       CNode{UserInt{0}}}}}}}}}}}}},
             {UserFieldname{"b"},
              CNode{
                  CNode::ObjectChildren{{KeyFieldname::orExpr,
                                         CNode{CNode::ArrayChildren{CNode{UserInt{1}},
                                                                    CNode{UserInt{2}},
                                                                    CNode{UserInt{3}},
                                                                    CNode{UserInt{4}}}}}}}}}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    auto& singleDoc = dynamic_cast<DocumentSourceSingleDocumentTransformation&>(**iter);
    // DocumenSourceSingleDoucmentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << true << "a"
                   << BSON("$and" << BSON_ARRAY(BSON("$const" << 1) << BSON(
                                                    "$add" << BSON_ARRAY(BSON("$const" << 1)
                                                                         << BSON("$const" << 0)))))
                   << "b"
                   << BSON("$or" << BSON_ARRAY(BSON("$const" << 1)
                                               << BSON("$const" << 2) << BSON("$const" << 3)
                                               << BSON("$const" << 4)))) ==
        singleDoc.getTransformer().serializeTransformation(boost::none).toBson()));
}

TEST(CstPipelineTranslationTest, TranslatesComputedProjectionWithExpressionOnId) {
    const auto cst = CNode{CNode::ArrayChildren{CNode{CNode::ObjectChildren{
        {KeyFieldname::project,
         CNode{CNode::ObjectChildren{
             {KeyFieldname::id,
              CNode{CNode::ObjectChildren{
                  {KeyFieldname::add,
                   CNode{CNode::ArrayChildren{
                       CNode{UserInt{0}},
                       CNode{CNode::ObjectChildren{
                           {KeyFieldname::andExpr,
                            CNode{CNode::ArrayChildren{CNode{UserInt{1}},
                                                       CNode{UserInt{0}}}}}}}}}}}}}}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    auto& singleDoc = dynamic_cast<DocumentSourceSingleDocumentTransformation&>(**iter);
    // DocumenSourceSingleDoucmentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << BSON(
                 "$add" << BSON_ARRAY(
                     BSON("$const" << 0)
                     << BSON("$and" << BSON_ARRAY(BSON("$const" << 1) << BSON("$const" << 0)))))) ==
        singleDoc.getTransformer().serializeTransformation(boost::none).toBson()));
}

TEST(CstPipelineTranslationTest, TranslatesSkipWithInt) {
    const auto cst = CNode{CNode::ArrayChildren{
        CNode{CNode::ObjectChildren{{KeyFieldname::skip, CNode{UserInt{5}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    ASSERT(typeid(DocumentSourceSkip) == typeid(**iter));
    ASSERT_EQ((dynamic_cast<DocumentSourceSkip&>(**iter).getSkip()), 5ll);
}

TEST(CstPipelineTranslationTest, TranslatesSkipWithDouble) {
    const auto cst = CNode{CNode::ArrayChildren{
        CNode{CNode::ObjectChildren{{KeyFieldname::skip, CNode{UserDouble{5.5}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    ASSERT(typeid(DocumentSourceSkip) == typeid(**iter));
    ASSERT_EQ((dynamic_cast<DocumentSourceSkip&>(**iter).getSkip()), 5ll);
}

TEST(CstPipelineTranslationTest, TranslatesSkipWithLong) {
    const auto cst = CNode{CNode::ArrayChildren{
        CNode{CNode::ObjectChildren{{KeyFieldname::skip, CNode{UserLong{8223372036854775807}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    ASSERT(typeid(DocumentSourceSkip) == typeid(**iter));
    ASSERT_EQ((dynamic_cast<DocumentSourceSkip&>(**iter).getSkip()), 8223372036854775807);
}

TEST(CstPipelineTranslationTest, TranslatesLimitWithInt) {
    const auto cst = CNode{CNode::ArrayChildren{
        CNode{CNode::ObjectChildren{{KeyFieldname::limit, CNode{UserInt{10}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    ASSERT(typeid(DocumentSourceLimit) == typeid(**iter));
    ASSERT_EQ(10ll, dynamic_cast<DocumentSourceLimit&>(**iter).getLimit());
}

TEST(CstPipelineTranslationTest, TranslatesLimitWithDouble) {
    const auto cst = CNode{CNode::ArrayChildren{
        CNode{CNode::ObjectChildren{{KeyFieldname::limit, CNode{UserDouble{10.5}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    ASSERT(typeid(DocumentSourceLimit) == typeid(**iter));
    ASSERT_EQ(10ll, dynamic_cast<DocumentSourceLimit&>(**iter).getLimit());
}

TEST(CstPipelineTranslationTest, TranslatesLimitWithLong) {
    const auto cst = CNode{CNode::ArrayChildren{
        CNode{CNode::ObjectChildren{{KeyFieldname::limit, CNode{UserLong{123123123123}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    ASSERT(typeid(DocumentSourceLimit) == typeid(**iter));
    ASSERT_EQ(123123123123, dynamic_cast<DocumentSourceLimit&>(**iter).getLimit());
}

TEST(CstPipelineTranslationTest, TranslatesCmpExpression) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::cmp,
         CNode{CNode::ArrayChildren{CNode{UserLong{1}}, CNode{UserDouble{2.5}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT(dynamic_cast<ExpressionCompare*>(expr.get()));
    ASSERT_EQ(ExpressionCompare::CmpOp::CMP, dynamic_cast<ExpressionCompare*>(expr.get())->getOp());
}

TEST(CstPipelineTranslationTest, TranslatesEqExpression) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::eq,
         CNode{CNode::ArrayChildren{CNode{UserLong{1}}, CNode{UserDouble{2.5}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT(dynamic_cast<ExpressionCompare*>(expr.get()));
    ASSERT_EQ(ExpressionCompare::CmpOp::EQ, dynamic_cast<ExpressionCompare*>(expr.get())->getOp());
}

TEST(CstPipelineTranslationTest, TranslatesGtExpression) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::gt,
         CNode{CNode::ArrayChildren{CNode{UserLong{1}}, CNode{UserDouble{2.5}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT(dynamic_cast<ExpressionCompare*>(expr.get()));
    ASSERT_EQ(ExpressionCompare::CmpOp::GT, dynamic_cast<ExpressionCompare*>(expr.get())->getOp());
}

TEST(CstPipelineTranslationTest, TranslatesGteExpression) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::gte,
         CNode{CNode::ArrayChildren{CNode{UserLong{1}}, CNode{UserDouble{2.5}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT(dynamic_cast<ExpressionCompare*>(expr.get()));
    ASSERT_EQ(ExpressionCompare::CmpOp::GTE, dynamic_cast<ExpressionCompare*>(expr.get())->getOp());
}

TEST(CstPipelineTranslationTest, TranslatesLtExpression) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::lt,
         CNode{CNode::ArrayChildren{CNode{UserLong{1}}, CNode{UserDouble{2.5}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT(dynamic_cast<ExpressionCompare*>(expr.get()));
    ASSERT_EQ(ExpressionCompare::CmpOp::LT, dynamic_cast<ExpressionCompare*>(expr.get())->getOp());
}

TEST(CstPipelineTranslationTest, TranslatesLteExpression) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::lte,
         CNode{CNode::ArrayChildren{CNode{UserLong{1}}, CNode{UserDouble{2.5}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT(dynamic_cast<ExpressionCompare*>(expr.get()));
    ASSERT_EQ(ExpressionCompare::CmpOp::LTE, dynamic_cast<ExpressionCompare*>(expr.get())->getOp());
}

TEST(CstPipelineTranslationTest, TranslatesNeExpression) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::ne,
         CNode{CNode::ArrayChildren{CNode{UserLong{1}}, CNode{UserDouble{2.5}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT(dynamic_cast<ExpressionCompare*>(expr.get()));
    ASSERT_EQ(ExpressionCompare::CmpOp::NE, dynamic_cast<ExpressionCompare*>(expr.get())->getOp());
}

}  // namespace
}  // namespace mongo
