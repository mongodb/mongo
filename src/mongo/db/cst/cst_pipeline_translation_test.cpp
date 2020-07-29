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
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_sample.h"
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

TEST(CstTest, TranslatesEmptyProject) {
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
    // DocumenSourceSingleDocumentTransformation reorders fields so we need to be insensitive.
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
    // DocumenSourceSingleDocumentTransformation reorders fields so we need to be insensitive.
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
    // DocumenSourceSingleDocumentTransformation reorders fields so we need to be insensitive.
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
    // DocumenSourceSingleDocumentTransformation reorders fields so we need to be insensitive.
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
    // DocumenSourceSingleDocumentTransformation reorders fields so we need to be insensitive.
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
    // DocumenSourceSingleDocumentTransformation reorders fields so we need to be insensitive.
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
        // DocumenSourceSingleDocumentTransformation reorders fields so we need to be insensitive.
        ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
            BSON("_id" << true << "a" << true) ==
            singleDoc.getTransformer().serializeTransformation(boost::none).toBson()));
    }
    {
        auto& singleDoc = dynamic_cast<DocumentSourceSingleDocumentTransformation&>(**iter++);
        // DocumenSourceSingleDocumentTransformation reorders fields so we need to be insensitive.
        ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
            BSON("b" << false) ==
            singleDoc.getTransformer().serializeTransformation(boost::none).toBson()));
    }
    {
        auto& singleDoc = dynamic_cast<DocumentSourceSingleDocumentTransformation&>(**iter);
        // DocumenSourceSingleDocumentTransformation reorders fields so we need to be insensitive.
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
        // DocumenSourceSingleDocumentTransformation reorders fields so we need to be
        // insensitive.
        ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
            BSON("_id" << true << "a" << BSON("$not" << BSON_ARRAY(BSON("$const" << 0)))) ==
            singleDoc.getTransformer().serializeTransformation(boost::none).toBson()));
    }
    {
        auto& singleDoc = dynamic_cast<DocumentSourceSingleDocumentTransformation&>(**iter);
        // DocumenSourceSingleDocumentTransformation reorders fields so we need to be
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
    // DocumenSourceSingleDocumentTransformation reorders fields so we need to be insensitive.
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
    // DocumenSourceSingleDocumentTransformation reorders fields so we need to be insensitive.
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

TEST(CstPipelineTranslationTest, FailsToTranslateSkipWithNegativeValue) {
    const auto cst = CNode{CNode::ArrayChildren{
        CNode{CNode::ObjectChildren{{KeyFieldname::skip, CNode{UserInt{-1}}}}}}};
    ASSERT_THROWS_CODE(
        cst_pipeline_translation::translatePipeline(cst, getExpCtx()), DBException, 15956);
}

TEST(CstPipelineTranslationTest, TranslatesLimitWithInt) {
    const auto cst = CNode{CNode::ArrayChildren{
        CNode{CNode::ObjectChildren{{KeyFieldname::skip, CNode{UserInt{0}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    ASSERT(typeid(DocumentSourceSkip) == typeid(**iter));
    ASSERT_EQ(0ll, dynamic_cast<DocumentSourceSkip&>(**iter).getSkip());
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

TEST(CstPipelineTranslationTest, FailsToTranslateLimitWithZeroKey) {
    const auto cst = CNode{CNode::ArrayChildren{
        CNode{CNode::ObjectChildren{{KeyFieldname::limit, CNode{UserInt{0}}}}}}};
    ASSERT_THROWS_CODE(
        cst_pipeline_translation::translatePipeline(cst, getExpCtx()), DBException, 15958);
}

TEST(CstPipelineTranslationTest, FailsToTranslateLimitWithNegativeValue) {
    const auto cst = CNode{CNode::ArrayChildren{
        CNode{CNode::ObjectChildren{{KeyFieldname::limit, CNode{UserInt{-1}}}}}}};
    ASSERT_THROWS_CODE(
        cst_pipeline_translation::translatePipeline(cst, getExpCtx()), DBException, 15958);
}

TEST(CstPipelineTranslationTest, TranslatesSampleWithValidSize) {
    {
        const auto cst = CNode{CNode::ArrayChildren{CNode{CNode::ObjectChildren{
            {KeyFieldname::sample,
             CNode{CNode::ObjectChildren{{KeyFieldname::sizeArg, CNode{UserLong{5}}}}}}}}}};
        auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
        auto& sources = pipeline->getSources();
        ASSERT_EQ(1u, sources.size());
        auto iter = sources.begin();
        ASSERT(typeid(DocumentSourceSample) == typeid(**iter));
        ASSERT_EQ(5ll, dynamic_cast<DocumentSourceSample&>(**iter).getSampleSize());
    }
    {
        const auto cst = CNode{CNode::ArrayChildren{CNode{CNode::ObjectChildren{
            {KeyFieldname::sample,
             CNode{CNode::ObjectChildren{{KeyFieldname::sizeArg, CNode{UserDouble{5.8}}}}}}}}}};
        auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
        auto& sources = pipeline->getSources();
        ASSERT_EQ(1u, sources.size());
        auto iter = sources.begin();
        ASSERT(typeid(DocumentSourceSample) == typeid(**iter));
        ASSERT_EQ(5ll, dynamic_cast<DocumentSourceSample&>(**iter).getSampleSize());
    }
    {
        const auto cst = CNode{CNode::ArrayChildren{CNode{CNode::ObjectChildren{
            {KeyFieldname::sample,
             CNode{CNode::ObjectChildren{{KeyFieldname::sizeArg, CNode{UserLong{0}}}}}}}}}};
        auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
        auto& sources = pipeline->getSources();
        ASSERT_EQ(1u, sources.size());
        auto iter = sources.begin();
        ASSERT(typeid(DocumentSourceSample) == typeid(**iter));
        ASSERT_EQ(0ll, dynamic_cast<DocumentSourceSample&>(**iter).getSampleSize());
    }
}

TEST(CstPipelineTranslationTest, FailsToTranslateSampleWithNegativeSize) {
    const auto cst = CNode{CNode::ArrayChildren{CNode{CNode::ObjectChildren{
        {KeyFieldname::sample,
         CNode{CNode::ObjectChildren{{KeyFieldname::sizeArg, CNode{UserInt{-1}}}}}}}}}};
    ASSERT_THROWS_CODE(
        cst_pipeline_translation::translatePipeline(cst, getExpCtx()), DBException, 28747);
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

TEST(CstPipelineTranslationTest, TranslatesProjectionWithConvert) {
    // [
    //     { $project: { a: { $convert: { input: "true", to: "bool"} },
    //                  { b: { $convert: { input: 1.999999, to: "int",
    //                                     onError: "Can't convert", onNull: NumberInt("1") } } }
    //     } }
    // ]
    const auto cst = CNode{CNode::ArrayChildren{CNode{CNode::ObjectChildren{
        {KeyFieldname::project,
         CNode{CNode::ObjectChildren{
             {UserFieldname{"a"},
              CNode{CNode::ObjectChildren{
                  {KeyFieldname::convert,
                   CNode{CNode::ObjectChildren{
                       {KeyFieldname::inputArg, CNode{UserBoolean{true}}},
                       {KeyFieldname::toArg, CNode{UserString{"bool"}}},
                       {KeyFieldname::onErrorArg, CNode{KeyValue::absentKey}},
                       {KeyFieldname::onNullArg, CNode{KeyValue::absentKey}}}}}}}},
             {UserFieldname{"b"},
              CNode{CNode::ObjectChildren{
                  {KeyFieldname::convert,
                   CNode{CNode::ObjectChildren{
                       {KeyFieldname::inputArg, CNode{UserDouble{1.999999}}},
                       {KeyFieldname::toArg, CNode{UserString{"int"}}},
                       {KeyFieldname::onErrorArg, CNode{UserString{"Can't convert"}}},
                       {KeyFieldname::onNullArg, CNode{UserInt{1}}}}}}}}}}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    auto& singleDoc = dynamic_cast<DocumentSourceSingleDocumentTransformation&>(**iter);
    // DocumenSourceSingleDocumentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << true << "a"
                   << BSON("$convert" << BSON("input" << BSON("$const" << true) << "to"
                                                      << BSON("$const"
                                                              << "bool")))
                   << "b"
                   << BSON("$convert" << BSON("input" << BSON("$const" << 1.999999) << "to"
                                                      << BSON("$const"
                                                              << "int")
                                                      << "onError"
                                                      << BSON("$const"
                                                              << "Can't convert")
                                                      << "onNull" << BSON("$const" << 1)))) ==
        singleDoc.getTransformer().serializeTransformation(boost::none).toBson()));
}

TEST(CstPipelineTranslationTest, TranslatesConvertExpression) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::convert,
         CNode{CNode::ObjectChildren{{KeyFieldname::inputArg, CNode{UserString{"true"}}},
                                     {KeyFieldname::toArg, CNode{UserString{"bool"}}},
                                     {KeyFieldname::onErrorArg, CNode{KeyValue::absentKey}},
                                     {KeyFieldname::onNullArg, CNode{UserInt{1}}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson(
            "{$convert: {input: {$const: 'true'}, to: {$const: 'bool'}, onNull: {$const: 1}}}")) ==
        expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, TranslatesToBoolExpression) {
    const auto cst = CNode{CNode::ObjectChildren{{KeyFieldname::toBool, CNode{UserInt{0}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$convert: {input: {$const: 0}, to: {$const: 'bool'}}}")) ==
        expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, TranslatesToDateExpression) {
    const auto cst = CNode{CNode::ObjectChildren{{KeyFieldname::toDate, CNode{UserLong{0}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$convert: {input: {$const: 0}, to: {$const: 'date'}}}")) ==
        expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, TranslatesToDecimalExpression) {
    const auto cst =
        CNode{CNode::ObjectChildren{{KeyFieldname::toDecimal, CNode{UserDouble{2.02}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$convert: {input: {$const: 2.02}, to: {$const: 'decimal'}}}")) ==
        expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, TranslatesToDoubleExpression) {
    const auto cst =
        CNode{CNode::ObjectChildren{{KeyFieldname::toDouble, CNode{UserString{"5.5"}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$convert: {input: {$const: '5.5'}, to: {$const: 'double'}}}")) ==
        expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, TranslatesToIntExpression) {
    const auto cst = CNode{CNode::ObjectChildren{{KeyFieldname::toInt, CNode{UserBoolean{true}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$convert: {input: {$const: true}, to: {$const: 'int'}}}")) ==
        expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, TranslatesToLongExpression) {
    const auto cst = CNode{CNode::ObjectChildren{{KeyFieldname::toLong, CNode{UserDecimal{1.0}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$convert: {input: {$const: 1.0}, to: {$const: 'long'}}}")) ==
        expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, TranslatesToObjectIdExpression) {
    const auto cst =
        CNode{CNode::ObjectChildren{{KeyFieldname::toObjectId, CNode{UserString{"$_id"}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$convert: {input: {$const: '$_id'}, to: {$const: 'objectId'}}}")) ==
        expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, TranslatesToStringExpression) {
    const auto cst =
        CNode{CNode::ObjectChildren{{KeyFieldname::toString, CNode{UserBoolean{true}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$convert: {input: {$const: true}, to: {$const: 'string'}}}")) ==
        expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, TranslatesTypeExpression) {
    const auto cst = CNode{CNode::ObjectChildren{{KeyFieldname::type, CNode{UserLong{1}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT(dynamic_cast<ExpressionType*>(expr.get()));
}


TEST(CstTest, AbsConstantTranslation) {
    auto cst = CNode{CNode::ObjectChildren{{KeyFieldname::abs, CNode{UserInt{-1}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(Value(fromjson("{$abs: [{$const: -1}]}")) ==
                                           expr->serialize(false)));
    cst = CNode{CNode::ObjectChildren{{KeyFieldname::abs, CNode{UserDouble{-1.534}}}}};
    expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(Value(fromjson("{$abs: [{$const: -1.534}]}")) ==
                                           expr->serialize(false)));
}

TEST(CstTest, AbsVariableTransation) {
    const auto cst = CNode{CNode::ObjectChildren{{KeyFieldname::abs, CNode{UserString{"$foo"}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    // TODO SERVER-49927 This should be a field path.
    ASSERT_TRUE(ValueComparator().evaluate(Value(fromjson("{$abs: [{$const: \"$foo\"}]}")) ==
                                           expr->serialize(false)));
}

TEST(CstTest, CeilTranslationTest) {
    const auto cst = CNode{CNode::ObjectChildren{{KeyFieldname::ceil, CNode{UserDouble{1.578}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(Value(fromjson("{$ceil: [{$const: 1.578}]}")) ==
                                           expr->serialize(false)));
}
TEST(CstTest, DivideTranslationTest) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::divide,
         CNode{CNode::ArrayChildren{CNode{UserDouble{1.5}}, CNode{UserDouble{1}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$divide: [{$const: 1.5}, {$const: 1}]}")) == expr->serialize(false)));
}

TEST(CstTest, ExpTranslationTest) {
    const auto cst = CNode{CNode::ObjectChildren{{KeyFieldname::exponent, CNode{UserDouble{1.5}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(Value(fromjson("{$exp: [{$const: 1.5}]}")) ==
                                           expr->serialize(false)));
}

TEST(CstTest, FloorTranslationTest) {
    const auto cst = CNode{CNode::ObjectChildren{{KeyFieldname::floor, CNode{UserDouble{1.5}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(Value(fromjson("{$floor: [{$const: 1.5}]}")) ==
                                           expr->serialize(false)));
}
TEST(CstTest, LnTranslationTest) {
    const auto cst = CNode{CNode::ObjectChildren{{KeyFieldname::ln, CNode{UserDouble{1.5}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(Value(fromjson("{$ln: [{$const: 1.5}]}")) ==
                                           expr->serialize(false)));
}

TEST(CstTest, LogTranslationTest) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::ln,
         CNode{CNode::ArrayChildren{CNode{UserDouble{1.5}}, CNode{UserDouble{10}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$ln: [{$const: 1.5}, {$const: 10}]}")) == expr->serialize(false)));
}

TEST(CstTest, LogTenTranslationTest) {
    const auto cst = CNode{CNode::ObjectChildren{{KeyFieldname::logten, CNode{UserDouble{1.5}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(Value(fromjson("{$log10: [{$const: 1.5}]}")) ==
                                           expr->serialize(false)));
}

TEST(CstTest, ModTranslationTest) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::mod,
         CNode{CNode::ArrayChildren{CNode{UserDouble{15}}, CNode{UserDouble{10}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$mod: [{$const: 15}, {$const: 10}]}")) == expr->serialize(false)));
}

TEST(CstTest, MultiplyTranslationTest) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::multiply,
         CNode{CNode::ArrayChildren{
             CNode{UserDouble{15}}, CNode{UserDouble{10}}, CNode{UserDouble{2}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$multiply: [{$const: 15}, {$const: 10}, {$const: 2}]}")) ==
        expr->serialize(false)));
}

TEST(CstTest, PowTranslationTest) {
    auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::pow,
         CNode{CNode::ArrayChildren{CNode{UserDouble{5}}, CNode{UserDouble{2}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(Value(fromjson("{$pow: [{$const: 5}, {$const: 2}]}")) ==
                                           expr->serialize(false)));
    cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::pow,
         CNode{CNode::ArrayChildren{CNode{UserDouble{5.846}}, CNode{UserDouble{2.846}}}}}}};
    expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$pow: [{$const: 5.846}, {$const: 2.846}]}")) == expr->serialize(false)));
}

TEST(CstTest, RoundTranslationTest) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::round,
         CNode{CNode::ArrayChildren{CNode{UserDouble{1.5786}}, CNode{UserDouble{2}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$round: [{$const: 1.5786}, {$const: 2}]}")) == expr->serialize(false)));
}

TEST(CstTest, SqrtTranslationTest) {
    const auto cst = CNode{CNode::ObjectChildren{{KeyFieldname::sqrt, CNode{UserDouble{144}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(Value(fromjson("{$sqrt: [{$const: 144}]}")) ==
                                           expr->serialize(false)));
}

TEST(CstTest, SubtractTranslationTest) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::subtract,
         CNode{CNode::ArrayChildren{CNode{UserDouble{1.5786}}, CNode{UserDouble{2}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$subtract: [{$const: 1.5786}, {$const: 2}]}")) == expr->serialize(false)));
}

TEST(CstTest, TruncTranslationTest) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::trunc,
         CNode{CNode::ArrayChildren{CNode{UserDouble{1.5786}}, CNode{UserDouble{2}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$trunc: [{$const: 1.5786}, {$const: 2}]}")) == expr->serialize(false)));
}

}  // namespace
}  // namespace mongo
