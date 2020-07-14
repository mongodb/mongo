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

TEST(CstTest, TranslatesEmpty) {
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

TEST(CstTest, TranslatesEmptyProjects) {
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

TEST(CstTest, TranslatesOneFieldInclusionProjectionStage) {
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

TEST(CstTest, TranslatesMultifieldInclusionProjection) {
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

TEST(CstTest, TranslatesOneFieldExclusionProjectionStage) {
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

TEST(CstTest, TranslatesMultifieldExclusionProjection) {
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

TEST(CstTest, FailsToTranslateInclusionExclusionMixedProjectionStage) {
    const auto cst = CNode{CNode::ArrayChildren{CNode{CNode::ObjectChildren{
        {KeyFieldname::project,
         CNode{CNode::ObjectChildren{{UserFieldname{"a"}, CNode{KeyValue::trueKey}},
                                     {UserFieldname{"b"}, CNode{KeyValue::falseKey}}}}}}}}};
    ASSERT_THROWS_CODE(
        cst_pipeline_translation::translatePipeline(cst, getExpCtx()), DBException, 4933100);
}

TEST(CstTest, TranslatesComputedProjection) {
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

TEST(CstTest, FailsToTranslateComputedExclusionMixedProjectionStage) {
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

TEST(CstTest, TranslatesComputedInclusionMixedProjectionStage) {
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

TEST(CstTest, TranslatesMultipleProjectionStages) {
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

TEST(CstTest, TranslatesSkipWithInt) {
    auto nss = NamespaceString{"db", "coll"};
    const auto cst = CNode{CNode::ArrayChildren{
        CNode{CNode::ObjectChildren{{KeyFieldname::skip, CNode{UserInt{5}}}}}}};
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest(nss));
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, expCtx);
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    ASSERT(typeid(DocumentSourceSkip) == typeid(**iter));
    ASSERT_EQ((dynamic_cast<DocumentSourceSkip&>(**iter).getSkip()), 5ll);
}

TEST(CstTest, TranslatesSkipWithDouble) {
    auto nss = NamespaceString{"db", "coll"};
    const auto cst = CNode{CNode::ArrayChildren{
        CNode{CNode::ObjectChildren{{KeyFieldname::skip, CNode{UserDouble{5.5}}}}}}};
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest(nss));
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, expCtx);
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    ASSERT(typeid(DocumentSourceSkip) == typeid(**iter));
    ASSERT_EQ((dynamic_cast<DocumentSourceSkip&>(**iter).getSkip()), 5ll);
}

TEST(CstTest, TranslatesSkipWithLong) {
    auto nss = NamespaceString{"db", "coll"};
    const auto cst = CNode{CNode::ArrayChildren{
        CNode{CNode::ObjectChildren{{KeyFieldname::skip, CNode{UserLong{8223372036854775807}}}}}}};
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest(nss));
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, expCtx);
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    ASSERT(typeid(DocumentSourceSkip) == typeid(**iter));
    ASSERT_EQ((dynamic_cast<DocumentSourceSkip&>(**iter).getSkip()), 8223372036854775807);
}

TEST(CstTest, TranslatesLimitWithInt) {
    auto nss = NamespaceString{"db", "coll"};
    const auto cst = CNode{CNode::ArrayChildren{
        CNode{CNode::ObjectChildren{{KeyFieldname::limit, CNode{UserInt{10}}}}}}};
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest(nss));
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, expCtx);
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    ASSERT(typeid(DocumentSourceLimit) == typeid(**iter));
    ASSERT_EQ(10ll, dynamic_cast<DocumentSourceLimit&>(**iter).getLimit());
}

TEST(CstTest, TranslatesLimitWithDouble) {
    auto nss = NamespaceString{"db", "coll"};
    const auto cst = CNode{CNode::ArrayChildren{
        CNode{CNode::ObjectChildren{{KeyFieldname::limit, CNode{UserDouble{10.5}}}}}}};
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest(nss));
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, expCtx);
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    ASSERT(typeid(DocumentSourceLimit) == typeid(**iter));
    ASSERT_EQ(10ll, dynamic_cast<DocumentSourceLimit&>(**iter).getLimit());
}

TEST(CstTest, TranslatesLimitWithLong) {
    auto nss = NamespaceString{"db", "coll"};
    const auto cst = CNode{CNode::ArrayChildren{
        CNode{CNode::ObjectChildren{{KeyFieldname::limit, CNode{UserLong{123123123123}}}}}}};
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest(nss));
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, expCtx);
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    ASSERT(typeid(DocumentSourceLimit) == typeid(**iter));
    ASSERT_EQ(123123123123, dynamic_cast<DocumentSourceLimit&>(**iter).getLimit());
}

}  // namespace
}  // namespace mongo
