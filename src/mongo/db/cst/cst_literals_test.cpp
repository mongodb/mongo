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

auto makePipelineContainingProjectStageWithLiteral(CNode&& literal) {
    return CNode{CNode::ArrayChildren{CNode{CNode::ObjectChildren{
        {KeyFieldname::project,
         CNode{CNode::ObjectChildren{
             {UserFieldname{"a"},
              CNode{CNode::ObjectChildren{{KeyFieldname::literal, std::move(literal)}}}}}}}}}}};
}

TEST(CstLiteralsTest, TranslatesDouble) {
    auto cst = makePipelineContainingProjectStageWithLiteral(CNode{UserDouble{5e-324}});
    // DocumenSourceSingleDoucmentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << true << "a" << BSON("$const" << 5e-324)) ==
        dynamic_cast<DocumentSourceSingleDocumentTransformation&>(
            **cst_pipeline_translation::translatePipeline(cst, getExpCtx())->getSources().begin())
            .getTransformer()
            .serializeTransformation(boost::none)
            .toBson()));
}

TEST(CstLiteralsTest, TranslatesString) {
    auto cst = makePipelineContainingProjectStageWithLiteral(CNode{UserString{"soup can"}});
    // DocumenSourceSingleDoucmentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << true << "a"
                   << BSON("$const"
                           << "soup can")) ==
        dynamic_cast<DocumentSourceSingleDocumentTransformation&>(
            **cst_pipeline_translation::translatePipeline(cst, getExpCtx())->getSources().begin())
            .getTransformer()
            .serializeTransformation(boost::none)
            .toBson()));
}

TEST(CstLiteralsTest, TranslatesBinary) {
    auto cst =
        makePipelineContainingProjectStageWithLiteral(CNode{UserBinary{"a\0b", 3, BinDataGeneral}});
    // DocumenSourceSingleDoucmentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << true << "a" << BSON("$const" << BSONBinData("a\0b", 3, BinDataGeneral))) ==
        dynamic_cast<DocumentSourceSingleDocumentTransformation&>(
            **cst_pipeline_translation::translatePipeline(cst, getExpCtx())->getSources().begin())
            .getTransformer()
            .serializeTransformation(boost::none)
            .toBson()));
}

TEST(CstLiteralsTest, TranslatesUndefined) {
    auto cst = makePipelineContainingProjectStageWithLiteral(CNode{UserUndefined{}});
    // DocumenSourceSingleDoucmentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << true << "a" << BSON("$const" << BSONUndefined)) ==
        dynamic_cast<DocumentSourceSingleDocumentTransformation&>(
            **cst_pipeline_translation::translatePipeline(cst, getExpCtx())->getSources().begin())
            .getTransformer()
            .serializeTransformation(boost::none)
            .toBson()));
}

TEST(CstLiteralsTest, TranslatesObjectId) {
    auto cst = makePipelineContainingProjectStageWithLiteral(
        CNode{UserObjectId{"01234567890123456789aaaa"}});
    // DocumenSourceSingleDoucmentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << true << "a" << BSON("$const" << OID("01234567890123456789aaaa"))) ==
        dynamic_cast<DocumentSourceSingleDocumentTransformation&>(
            **cst_pipeline_translation::translatePipeline(cst, getExpCtx())->getSources().begin())
            .getTransformer()
            .serializeTransformation(boost::none)
            .toBson()));
}

TEST(CstLiteralsTest, TranslatesBoolean) {
    auto cst = makePipelineContainingProjectStageWithLiteral(CNode{UserBoolean{false}});
    // DocumenSourceSingleDoucmentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << true << "a" << BSON("$const" << false)) ==
        dynamic_cast<DocumentSourceSingleDocumentTransformation&>(
            **cst_pipeline_translation::translatePipeline(cst, getExpCtx())->getSources().begin())
            .getTransformer()
            .serializeTransformation(boost::none)
            .toBson()));
}

TEST(CstLiteralsTest, TranslatesDate) {
    auto cst = makePipelineContainingProjectStageWithLiteral(
        CNode{UserDate{Date_t::fromMillisSinceEpoch(424242)}});
    // DocumenSourceSingleDoucmentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << true << "a" << BSON("$const" << Date_t::fromMillisSinceEpoch(424242))) ==
        dynamic_cast<DocumentSourceSingleDocumentTransformation&>(
            **cst_pipeline_translation::translatePipeline(cst, getExpCtx())->getSources().begin())
            .getTransformer()
            .serializeTransformation(boost::none)
            .toBson()));
}

TEST(CstLiteralsTest, TranslatesNull) {
    auto cst = makePipelineContainingProjectStageWithLiteral(CNode{UserNull{}});
    // DocumenSourceSingleDoucmentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << true << "a" << BSON("$const" << BSONNULL)) ==
        dynamic_cast<DocumentSourceSingleDocumentTransformation&>(
            **cst_pipeline_translation::translatePipeline(cst, getExpCtx())->getSources().begin())
            .getTransformer()
            .serializeTransformation(boost::none)
            .toBson()));
}

TEST(CstLiteralsTest, TranslatesRegex) {
    auto cst = makePipelineContainingProjectStageWithLiteral(CNode{UserRegex{".*", "i"}});
    // DocumenSourceSingleDoucmentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << true << "a" << BSON("$const" << BSONRegEx(".*", "i"))) ==
        dynamic_cast<DocumentSourceSingleDocumentTransformation&>(
            **cst_pipeline_translation::translatePipeline(cst, getExpCtx())->getSources().begin())
            .getTransformer()
            .serializeTransformation(boost::none)
            .toBson()));
}

TEST(CstLiteralsTest, TranslatesDBPointer) {
    auto cst = makePipelineContainingProjectStageWithLiteral(
        CNode{UserDBPointer{"db.c", OID("010203040506070809101112")}});
    // DocumenSourceSingleDoucmentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << true << "a"
                   << BSON("$const" << BSONDBRef("db.c", OID("010203040506070809101112")))) ==
        dynamic_cast<DocumentSourceSingleDocumentTransformation&>(
            **cst_pipeline_translation::translatePipeline(cst, getExpCtx())->getSources().begin())
            .getTransformer()
            .serializeTransformation(boost::none)
            .toBson()));
}

TEST(CstLiteralsTest, TranslatesJavascript) {
    auto cst = makePipelineContainingProjectStageWithLiteral(CNode{UserJavascript{"5 === 5"}});
    // DocumenSourceSingleDoucmentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << true << "a" << BSON("$const" << BSONCode("5 === 5"))) ==
        dynamic_cast<DocumentSourceSingleDocumentTransformation&>(
            **cst_pipeline_translation::translatePipeline(cst, getExpCtx())->getSources().begin())
            .getTransformer()
            .serializeTransformation(boost::none)
            .toBson()));
}

TEST(CstLiteralsTest, TranslatesSymbol) {
    auto cst = makePipelineContainingProjectStageWithLiteral(CNode{UserSymbol{"foo"}});
    // DocumenSourceSingleDoucmentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << true << "a" << BSON("$const" << BSONSymbol("foo"))) ==
        dynamic_cast<DocumentSourceSingleDocumentTransformation&>(
            **cst_pipeline_translation::translatePipeline(cst, getExpCtx())->getSources().begin())
            .getTransformer()
            .serializeTransformation(boost::none)
            .toBson()));
}

TEST(CstLiteralsTest, TranslatesJavascriptWithScope) {
    auto cst = makePipelineContainingProjectStageWithLiteral(
        CNode{UserJavascriptWithScope{"6 === 6", BSONObj{}}});
    // DocumenSourceSingleDoucmentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << true << "a" << BSON("$const" << BSONCodeWScope("6 === 6", BSONObj()))) ==
        dynamic_cast<DocumentSourceSingleDocumentTransformation&>(
            **cst_pipeline_translation::translatePipeline(cst, getExpCtx())->getSources().begin())
            .getTransformer()
            .serializeTransformation(boost::none)
            .toBson()));
}

TEST(CstLiteralsTest, TranslatesInt) {
    auto cst = makePipelineContainingProjectStageWithLiteral(CNode{UserInt{777}});
    // DocumenSourceSingleDoucmentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << true << "a" << BSON("$const" << 777)) ==
        dynamic_cast<DocumentSourceSingleDocumentTransformation&>(
            **cst_pipeline_translation::translatePipeline(cst, getExpCtx())->getSources().begin())
            .getTransformer()
            .serializeTransformation(boost::none)
            .toBson()));
}

TEST(CstLiteralsTest, TranslatesTimestamp) {
    auto cst = makePipelineContainingProjectStageWithLiteral(CNode{UserTimestamp{4102444800, 1}});
    // DocumenSourceSingleDoucmentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << true << "a" << BSON("$const" << Timestamp(4102444800, 1))) ==
        dynamic_cast<DocumentSourceSingleDocumentTransformation&>(
            **cst_pipeline_translation::translatePipeline(cst, getExpCtx())->getSources().begin())
            .getTransformer()
            .serializeTransformation(boost::none)
            .toBson()));
}

TEST(CstLiteralsTest, TranslatesLong) {
    auto cst = makePipelineContainingProjectStageWithLiteral(CNode{UserLong{777777777777777777ll}});
    // DocumenSourceSingleDoucmentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << true << "a" << BSON("$const" << 777777777777777777ll)) ==
        dynamic_cast<DocumentSourceSingleDocumentTransformation&>(
            **cst_pipeline_translation::translatePipeline(cst, getExpCtx())->getSources().begin())
            .getTransformer()
            .serializeTransformation(boost::none)
            .toBson()));
}

TEST(CstLiteralsTest, TranslatesDecimal) {
    auto cst = makePipelineContainingProjectStageWithLiteral(
        CNode{UserDecimal{Decimal128::kLargestNegative}});
    // DocumenSourceSingleDoucmentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << true << "a" << BSON("$const" << Decimal128::kLargestNegative)) ==
        dynamic_cast<DocumentSourceSingleDocumentTransformation&>(
            **cst_pipeline_translation::translatePipeline(cst, getExpCtx())->getSources().begin())
            .getTransformer()
            .serializeTransformation(boost::none)
            .toBson()));
}

TEST(CstLiteralsTest, TranslatesMinKey) {
    auto cst = makePipelineContainingProjectStageWithLiteral(CNode{UserMinKey{}});
    // DocumenSourceSingleDoucmentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << true << "a" << BSON("$const" << MINKEY)) ==
        dynamic_cast<DocumentSourceSingleDocumentTransformation&>(
            **cst_pipeline_translation::translatePipeline(cst, getExpCtx())->getSources().begin())
            .getTransformer()
            .serializeTransformation(boost::none)
            .toBson()));
}

TEST(CstLiteralsTest, TranslatesMaxKey) {
    auto cst = makePipelineContainingProjectStageWithLiteral(CNode{UserMaxKey{}});
    // DocumenSourceSingleDoucmentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << true << "a" << BSON("$const" << MAXKEY)) ==
        dynamic_cast<DocumentSourceSingleDocumentTransformation&>(
            **cst_pipeline_translation::translatePipeline(cst, getExpCtx())->getSources().begin())
            .getTransformer()
            .serializeTransformation(boost::none)
            .toBson()));
}

TEST(CstLiteralsTest, TranslatesArray) {
    auto cst = makePipelineContainingProjectStageWithLiteral(CNode{});
    // DocumenSourceSingleDoucmentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << true << "a" << BSON("$const" << BSONArray())) ==
        dynamic_cast<DocumentSourceSingleDocumentTransformation&>(
            **cst_pipeline_translation::translatePipeline(cst, getExpCtx())->getSources().begin())
            .getTransformer()
            .serializeTransformation(boost::none)
            .toBson()));
}

TEST(CstLiteralsTest, TranslatesObject) {
    auto cst = makePipelineContainingProjectStageWithLiteral(CNode{CNode::ObjectChildren{}});
    // DocumenSourceSingleDoucmentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << true << "a" << BSON("$const" << BSONObj())) ==
        dynamic_cast<DocumentSourceSingleDocumentTransformation&>(
            **cst_pipeline_translation::translatePipeline(cst, getExpCtx())->getSources().begin())
            .getTransformer()
            .serializeTransformation(boost::none)
            .toBson()));
}

TEST(CstLiteralsTest, TranslatesNestedLiteral) {
    auto cst = makePipelineContainingProjectStageWithLiteral(CNode{CNode::ObjectChildren{
        {UserFieldname{"a"}, CNode{UserMaxKey{}}},
        {UserFieldname{"b"},
         CNode{CNode::ObjectChildren{{UserFieldname{"1"}, CNode{UserDecimal{1.0}}},
                                     {UserFieldname{"2"}, CNode{UserLong{2ll}}}}}},
        {UserFieldname{"c"},
         CNode{CNode::ArrayChildren{CNode{UserString{"foo"}}, CNode{UserSymbol{"bar"}}}}}}});
    // DocumenSourceSingleDoucmentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << true << "a"
                   << BSON("$const" << BSON("a" << MAXKEY << "b"
                                                << BSON("1" << Decimal128(1.0) << "2" << 2ll) << "c"
                                                << BSON_ARRAY("foo" << BSONSymbol("bar"))))) ==
        dynamic_cast<DocumentSourceSingleDocumentTransformation&>(
            **cst_pipeline_translation::translatePipeline(cst, getExpCtx())->getSources().begin())
            .getTransformer()
            .serializeTransformation(boost::none)
            .toBson()));
}

}  // namespace
}  // namespace mongo
