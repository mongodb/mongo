/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <boost/optional.hpp>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands/map_reduce_agg.h"
#include "mongo/db/commands/mr_common.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_merge.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using namespace std::string_literals;
using namespace map_reduce_agg;

// The translator treats Javascript objects as black boxes so there's no need for realistic examples
// here.
constexpr auto initJavascript = "init!"_sd;
constexpr auto mapJavascript = "map!"_sd;
constexpr auto reduceJavascript = "reduce!"_sd;
constexpr auto finalizeJavascript = "finalize!"_sd;

TEST(MapReduceAggTest, testBasicTranslate) {
    auto nss = NamespaceString{"db", "coll"};
    auto mr =
        MapReduceCommandRequest{nss,
                                MapReduceJavascriptCode{mapJavascript.toString()},
                                MapReduceJavascriptCode{reduceJavascript.toString()},
                                MapReduceOutOptions{boost::none, "", OutputType::InMemory, false}};
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest(nss));
    auto pipeline = map_reduce_common::translateFromMR(mr, expCtx);
    auto& sources = pipeline->getSources();
    ASSERT_EQ(3u, sources.size());
    auto iter = sources.begin();
    ASSERT(typeid(DocumentSourceSingleDocumentTransformation) == typeid(**iter++));
    ASSERT(typeid(DocumentSourceUnwind) == typeid(**iter++));
    ASSERT(typeid(DocumentSourceGroup) == typeid(**iter));
}

TEST(MapReduceAggTest, testSortWithoutLimit) {
    auto nss = NamespaceString{"db", "coll"};
    auto mr =
        MapReduceCommandRequest{nss,
                                MapReduceJavascriptCode{mapJavascript.toString()},
                                MapReduceJavascriptCode{reduceJavascript.toString()},
                                MapReduceOutOptions{boost::none, "", OutputType::InMemory, false}};
    mr.setSort(BSON("foo" << 1));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest(nss));
    auto pipeline = map_reduce_common::translateFromMR(mr, expCtx);
    auto& sources = pipeline->getSources();
    ASSERT_EQ(4u, sources.size());
    auto iter = sources.begin();
    ASSERT(typeid(DocumentSourceSort) == typeid(**iter));
    auto& sort = dynamic_cast<DocumentSourceSort&>(**iter++);
    ASSERT(!sort.hasLimit());
    ASSERT(typeid(DocumentSourceSingleDocumentTransformation) == typeid(**iter++));
    ASSERT(typeid(DocumentSourceUnwind) == typeid(**iter++));
    ASSERT(typeid(DocumentSourceGroup) == typeid(**iter));
}

TEST(MapReduceAggTest, testSortWithLimit) {
    auto nss = NamespaceString{"db", "coll"};
    auto mr =
        MapReduceCommandRequest{nss,
                                MapReduceJavascriptCode{mapJavascript.toString()},
                                MapReduceJavascriptCode{reduceJavascript.toString()},
                                MapReduceOutOptions{boost::none, "", OutputType::InMemory, false}};
    mr.setSort(BSON("foo" << 1));
    mr.setLimit(23);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest(nss));
    auto pipeline = map_reduce_common::translateFromMR(mr, expCtx);
    auto& sources = pipeline->getSources();

    // Even though we specify a limit, there will only be 4 stages since the optimizer should
    // merge $sort and $limit.
    ASSERT_EQ(4u, sources.size());
    auto iter = sources.begin();
    ASSERT(typeid(DocumentSourceSort) == typeid(**iter));
    auto& sort = dynamic_cast<DocumentSourceSort&>(**iter++);
    ASSERT_EQ(23LL, *sort.getLimit());
    ASSERT(typeid(DocumentSourceSingleDocumentTransformation) == typeid(**iter++));
    ASSERT(typeid(DocumentSourceUnwind) == typeid(**iter++));
    ASSERT(typeid(DocumentSourceGroup) == typeid(**iter));
}

TEST(MapReduceAggTest, testLimitNoSort) {
    auto nss = NamespaceString{"db", "coll"};
    auto mr =
        MapReduceCommandRequest{nss,
                                MapReduceJavascriptCode{mapJavascript.toString()},
                                MapReduceJavascriptCode{reduceJavascript.toString()},
                                MapReduceOutOptions{boost::none, "", OutputType::InMemory, false}};
    mr.setLimit(23);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest(nss));
    auto pipeline = map_reduce_common::translateFromMR(mr, expCtx);
    auto& sources = pipeline->getSources();
    ASSERT_EQ(4u, sources.size());
    auto iter = sources.begin();
    ASSERT(typeid(DocumentSourceLimit) == typeid(**iter));
    auto& limit = dynamic_cast<DocumentSourceLimit&>(**iter++);
    ASSERT_EQ(23LL, limit.getLimit());
    ASSERT(typeid(DocumentSourceSingleDocumentTransformation) == typeid(**iter++));
    ASSERT(typeid(DocumentSourceUnwind) == typeid(**iter++));
    ASSERT(typeid(DocumentSourceGroup) == typeid(**iter));
}

TEST(MapReduceAggTest, testFeatureLadenTranslate) {
    auto nss = NamespaceString{"db", "coll"};
    auto mr = MapReduceCommandRequest{
        nss,
        MapReduceJavascriptCode{mapJavascript.toString()},
        MapReduceJavascriptCode{reduceJavascript.toString()},
        MapReduceOutOptions{boost::make_optional("db"s), "coll2", OutputType::Replace, false}};
    mr.setSort(BSON("foo" << 1));
    mr.setQuery(BSON("foo"
                     << "fooval"));
    mr.setFinalize(
        boost::make_optional(MapReduceJavascriptCodeOrNull{finalizeJavascript.toString()}));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest(nss));
    auto pipeline = map_reduce_common::translateFromMR(mr, expCtx);
    auto& sources = pipeline->getSources();
    ASSERT_EQ(7u, sources.size());
    auto iter = sources.begin();
    ASSERT(typeid(DocumentSourceMatch) == typeid(**iter++));
    ASSERT(typeid(DocumentSourceSort) == typeid(**iter++));
    ASSERT(typeid(DocumentSourceSingleDocumentTransformation) == typeid(**iter++));
    ASSERT(typeid(DocumentSourceUnwind) == typeid(**iter++));
    ASSERT(typeid(DocumentSourceGroup) == typeid(**iter++));
    ASSERT(typeid(DocumentSourceSingleDocumentTransformation) == typeid(**iter++));
    ASSERT(typeid(DocumentSourceOut) == typeid(**iter));
}

TEST(MapReduceAggTest, testOutMergeTranslate) {
    auto nss = NamespaceString{"db", "coll"};
    auto mr = MapReduceCommandRequest{
        nss,
        MapReduceJavascriptCode{mapJavascript.toString()},
        MapReduceJavascriptCode{reduceJavascript.toString()},
        MapReduceOutOptions{boost::make_optional("db"s), "coll2", OutputType::Merge, false}};
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest(nss));
    auto pipeline = map_reduce_common::translateFromMR(mr, expCtx);
    auto& sources = pipeline->getSources();
    ASSERT_EQ(sources.size(), 4u);
    auto iter = sources.begin();
    ASSERT(typeid(DocumentSourceSingleDocumentTransformation) == typeid(**iter++));
    ASSERT(typeid(DocumentSourceUnwind) == typeid(**iter++));
    ASSERT(typeid(DocumentSourceGroup) == typeid(**iter++));
    ASSERT(typeid(DocumentSourceMerge) == typeid(**iter));
    auto& merge = dynamic_cast<DocumentSourceMerge&>(**iter);
    ASSERT_FALSE(merge.getPipeline());
}

TEST(MapReduceAggTest, testOutReduceTranslate) {
    auto nss = NamespaceString{"db", "coll"};
    auto mr = MapReduceCommandRequest{
        nss,
        MapReduceJavascriptCode{mapJavascript.toString()},
        MapReduceJavascriptCode{reduceJavascript.toString()},
        MapReduceOutOptions{boost::make_optional("db"s), "coll2", OutputType::Reduce, false}};
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest(nss));
    auto pipeline = map_reduce_common::translateFromMR(mr, expCtx);
    auto& sources = pipeline->getSources();
    ASSERT_EQ(sources.size(), 4u);
    auto iter = sources.begin();
    ASSERT(typeid(DocumentSourceSingleDocumentTransformation) == typeid(**iter++));
    ASSERT(typeid(DocumentSourceUnwind) == typeid(**iter++));
    ASSERT(typeid(DocumentSourceGroup) == typeid(**iter++));
    ASSERT(typeid(DocumentSourceMerge) == typeid(**iter));
    auto& merge = dynamic_cast<DocumentSourceMerge&>(**iter);
    auto subpipeline = merge.getPipeline();
    ASSERT_EQ(1u, subpipeline->size());
    ASSERT_EQ("$project"s, (*subpipeline)[0].firstElement().fieldName());
}

TEST(MapReduceAggTest, testOutSameCollection) {
    auto nss = NamespaceString{"db", "coll"};
    auto mr = MapReduceCommandRequest{
        nss,
        MapReduceJavascriptCode{mapJavascript.toString()},
        MapReduceJavascriptCode{reduceJavascript.toString()},
        MapReduceOutOptions{boost::make_optional("db"s), "coll", OutputType::Replace, false}};
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest(nss));
    auto pipeline = map_reduce_common::translateFromMR(mr, expCtx);
    auto& sources = pipeline->getSources();
    ASSERT_EQ(sources.size(), 4u);
    auto iter = sources.begin();
    ASSERT(typeid(DocumentSourceSingleDocumentTransformation) == typeid(**iter++));
    ASSERT(typeid(DocumentSourceUnwind) == typeid(**iter++));
    ASSERT(typeid(DocumentSourceGroup) == typeid(**iter++));
    ASSERT(typeid(DocumentSourceOut) == typeid(**iter));
}

TEST(MapReduceAggTest, testSourceDestinationCollectionsEqualMergeDoesNotFail) {
    auto nss = NamespaceString{"db", "coll"};
    auto mr = MapReduceCommandRequest{
        nss,
        MapReduceJavascriptCode{mapJavascript.toString()},
        MapReduceJavascriptCode{reduceJavascript.toString()},
        MapReduceOutOptions{boost::make_optional("db"s), "coll", OutputType::Merge, false}};
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest(nss));
    ASSERT_DOES_NOT_THROW(map_reduce_common::translateFromMR(mr, expCtx));
}

TEST(MapReduceAggTest, testSourceDestinationCollectionsNotEqualMergeDoesNotFail) {
    auto nss = NamespaceString{"db", "coll"};
    auto mr = MapReduceCommandRequest{
        nss,
        MapReduceJavascriptCode{mapJavascript.toString()},
        MapReduceJavascriptCode{reduceJavascript.toString()},
        MapReduceOutOptions{boost::make_optional("db2"s), "coll", OutputType::Merge, false}};
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest(nss));
    ASSERT_DOES_NOT_THROW(map_reduce_common::translateFromMR(mr, expCtx));
}

TEST(MapReduceAggTest, testShardedTrueWithReplaceActionIsNotAllowed) {
    auto nss = NamespaceString{"db", "coll"};
    auto mr = MapReduceCommandRequest{
        nss,
        MapReduceJavascriptCode{mapJavascript.toString()},
        MapReduceJavascriptCode{reduceJavascript.toString()},
        MapReduceOutOptions{boost::make_optional("db"s), "coll2", OutputType::Replace, true}};
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest(nss));
    ASSERT_THROWS_CODE(
        map_reduce_common::translateFromMR(mr, expCtx), DBException, ErrorCodes::InvalidOptions);
}

TEST(MapReduceAggTest, testErrorMessagesTranslated) {
    // Verifies that agg specific error messages are translated to be mapReduce specific.
    auto nss = NamespaceString{"db", "coll1"};

    auto mr = MapReduceCommandRequest{
        nss,
        MapReduceJavascriptCode{mapJavascript.toString()},
        MapReduceJavascriptCode{reduceJavascript.toString()},
        MapReduceOutOptions{boost::make_optional("db"s), "coll2", OutputType::Merge, false}};
    mr.setLimit(-23);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest(nss));
    ASSERT_THROWS_CODE_AND_WHAT(map_reduce_common::translateFromMR(mr, expCtx),
                                DBException,
                                15958,
                                "The limit specified to mapReduce must be positive");
}


}  // namespace
}  // namespace mongo
