/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/change_stream_rewrite_helpers.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/bson/bson_helper.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <memory>
#include <string>
#include <vector>

#include <boost/algorithm/string/join.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {
using namespace std::string_literals;

class ChangeStreamRewriteTest : public AggregationContextFixture {
public:
    ChangeStreamRewriteTest()
        : AggregationContextFixture(NamespaceString::createNamespaceString_forTest(
              boost::none, "unittests", "pipeline_test")) {}

    std::string getNsDbRegexMatchExpr(const std::string& field, const std::string& regex) {
        return str::stream()
            << "{$expr: {$let: {vars: {oplogField: {$cond: [{ $eq: [{ $type: ['" << field
            << "']}, {$const: 'string'}]}, '" << field
            << "', '$$REMOVE']}}, in: {$regexMatch: {input: {$substrBytes: ['$$oplogField', "
               "{$const: 0}, {$ifNull: [{$indexOfBytes: ['$$oplogField', {$const: "
               "'.'}]}, {$const: 0}]}]}, regex: {$const: '"
            << regex << "'}, options: {$const: ''}}}}}}";
    }

    std::string getNsCollRegexMatchExpr(const std::string& field, const std::string& regex) {
        if (field == "$o.drop" || field == "$o.create" || field == "$o.createIndexes" ||
            field == "$o.commitIndexBuild" || field == "$o.startIndexBuild" ||
            field == "$o.abortIndexBuild" || field == "$o.dropIndexes" || field == "$o.collMod") {
            return str::stream()
                << "{$expr: {$let: {vars: {oplogField: {$cond: [{ $eq: [{ $type: ['" << field
                << "']}, {$const: 'string'}]}, '" << field
                << "', '$$REMOVE']}}, in: {$regexMatch: {input: '$$oplogField', regex: {$const: '"
                << regex << "'}, options: {$const: ''}}}}}}";
        }

        return str::stream()
            << "{$expr: {$let: {vars: {oplogField: {$cond: [{ $eq: [{ $type: ['" << field
            << "']}, {$const: 'string'}]}, '" << field
            << "', '$$REMOVE']}}, in: {$regexMatch: {input: {$substrBytes: ['$$oplogField', {$add: "
               "[{$const: 1}, "
               "{$ifNull: [{$indexOfBytes: ['$$oplogField', {$const: '.'}]}, {$const: 0}]}]}, "
               "{$const: -1}]}, regex: {$const: '"
            << regex << "'}, options: {$const: ''}} }}}}";
    }
};

//
// Logical rewrites
//
TEST_F(ChangeStreamRewriteTest, RewriteOrPredicateOnRenameableFields) {
    auto spec = fromjson("{$or: [{clusterTime: {$type: [17]}}, {lsid: {$type: [16]}}]}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression =
        change_stream_rewrite::rewriteFilterForFields(getExpCtx(),
                                                      statusWithMatchExpression.getValue().get(),
                                                      bsonObjsArray,
                                                      {"clusterTime", "lsid"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$or: [{ts: {$type: [17]}}, {lsid: {$type: [16]}}]}"));
}

TEST_F(ChangeStreamRewriteTest, InexactRewriteOfAndWithUnrewritableChild) {
    // Note that rewrite of {operationType: {$gt: ...}} is currently not supported.
    auto spec = fromjson("{$and: [{lsid: {$type: [16]}}, {operationType: {$gt: 0}}]}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression =
        change_stream_rewrite::rewriteFilterForFields(getExpCtx(),
                                                      statusWithMatchExpression.getValue().get(),
                                                      bsonObjsArray,
                                                      {"clusterTime", "lsid", "operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, fromjson("{$and: [{lsid: {$type: [16]}}]}"));
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteOrWithUnrewritableChild) {
    // Note that rewrite of {operationType: {$gt: ...}} is currently not supported.
    auto spec = fromjson(
        "{$and: [{clusterTime: {$type: [17]}}, "
        "{$or: [{lsid: {$type: [16]}}, {operationType: {$gt: 0}}]}]}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression =
        change_stream_rewrite::rewriteFilterForFields(getExpCtx(),
                                                      statusWithMatchExpression.getValue().get(),
                                                      bsonObjsArray,
                                                      {"clusterTime", "lsid", "operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, fromjson("{$and: [{ts: {$type: [17]}}]}"));
}

TEST_F(ChangeStreamRewriteTest, InexactRewriteOfNorWithUnrewritableChild) {
    // Note that rewrite of {operationType: {$gt: ...}} is currently not supported.
    auto spec = fromjson(
        "{$and: [{clusterTime: {$type: [17]}}, "
        "{$nor: [{lsid: {$type: [16]}}, {operationType: {$gt: 0}}]}]}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression =
        change_stream_rewrite::rewriteFilterForFields(getExpCtx(),
                                                      statusWithMatchExpression.getValue().get(),
                                                      bsonObjsArray,
                                                      {"clusterTime", "lsid", "operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: [{ts: {$type: [17]}}, {$nor: [{lsid: {$type: [16]}}]}]}"));
}

TEST_F(ChangeStreamRewriteTest, CannotInexactlyRewriteNegatedPredicate) {
    // Note that rewrite of {operationType: {$gt: ...}} is currently not supported. The rewrite must
    // discard the _entire_ $and, because it is the child of $nor, and the rewrite cannot use
    // inexact rewrites of any children of $not or $nor.
    auto spec = fromjson(
        "{$nor: [{clusterTime: {$type: [17]}}, "
        "{$and: [{lsid: {$type: [16]}}, {operationType: {$gt: 0}}]}]}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression =
        change_stream_rewrite::rewriteFilterForFields(getExpCtx(),
                                                      statusWithMatchExpression.getValue().get(),
                                                      bsonObjsArray,
                                                      {"operationType", "clusterTime", "lsid"});

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, fromjson("{$nor: [{ts: {$type: [17]}}]}"));
}

TEST_F(ChangeStreamRewriteTest, DoesNotRewriteUnrequestedField) {
    // This 'operationType' predicate could be rewritten but will be discarded in this test because
    // the 'operationType' field is not requested in the call to the 'rewriteFilterForFields()'
    // function.
    auto spec = fromjson("{operationType: 'insert', clusterTime: {$type: [17]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression =
        change_stream_rewrite::rewriteFilterForFields(getExpCtx(),
                                                      statusWithMatchExpression.getValue().get(),
                                                      bsonObjsArray,
                                                      {"clusterTime", "lsid"});

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, fromjson("{$and: [{ts: {$type: [17]}}]}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteExprWhenAllFieldsAreRenameable) {
    auto spec = fromjson(
        "{$expr: {$and: [{$eq: [{$tsSecond: '$clusterTime'}, 0]}, {$isNumber: '$lsid'}]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression =
        change_stream_rewrite::rewriteFilterForFields(getExpCtx(),
                                                      statusWithMatchExpression.getValue().get(),
                                                      bsonObjsArray,
                                                      {"clusterTime", "lsid"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$expr: {$and: [{$eq: [{$tsSecond: ['$ts']}, {$const: 0}]}, "
                               "{$isNumber: ['$lsid']}]}}"));
}

TEST_F(ChangeStreamRewriteTest, CanInexactlyRewriteExprAndWithUnrewritableChild) {
    // Note that rewrite of "$$ROOT" without a path is unsupported.
    auto spec = fromjson("{$expr: {$and: [{$isNumber: '$lsid'}, {$isArray: '$$ROOT'}]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"lsid"});

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, fromjson("{$expr: {$and: [{$isNumber: ['$lsid']}]}}"));
}

TEST_F(ChangeStreamRewriteTest, CanInexactlyRewriteExprAndWithUnknownField) {
    // $expr cannot be split into individual expressions based on dependency analysis, but because
    // we rewrite the entire tree and discard expressions which we cannot rewrite or which the user
    // did not request, we are able to produce a correct output expression even when unknown fields
    // exist.
    auto spec = fromjson("{$expr: {$and: [{$isNumber: '$lsid'}, {$isArray: '$unknown'}]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"lsid"});

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, fromjson("{$expr: {$and: [{$isNumber: ['$lsid']}]}}"));
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteExprOrWithUnrewritableChild) {
    // Note that rewrite of "$$ROOT" without a path is unsupported.
    auto spec = fromjson(
        "{$expr: {$and: [{$isNumber: '$clusterTime'}, "
        "{$or: [{$isNumber: '$lsid'}, {$isArray: '$$ROOT'}]}]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression =
        change_stream_rewrite::rewriteFilterForFields(getExpCtx(),
                                                      statusWithMatchExpression.getValue().get(),
                                                      bsonObjsArray,
                                                      {"clusterTime", "lsid"});

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, fromjson("{$expr: {$and: [{$isNumber: ['$ts']}]}}"));
}

TEST_F(ChangeStreamRewriteTest, CanInexactlyRewriteExprNorWithUnrewritableChild) {
    // Note that rewrite of "$$ROOT" without a path is unsupported.
    auto spec = fromjson(
        "{$expr: {$and: [{$isNumber: '$clusterTime'},"
        "{$not: {$or: [{$isNumber: '$lsid'}, {$isArray: '$$ROOT'}]}}]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression =
        change_stream_rewrite::rewriteFilterForFields(getExpCtx(),
                                                      statusWithMatchExpression.getValue().get(),
                                                      bsonObjsArray,
                                                      {"clusterTime", "lsid"});

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        fromjson(
            "{$expr: {$and: [{$isNumber: ['$ts']}, {$not: [{$or: [{$isNumber: ['$lsid']}]}]}]}}"));
}

TEST_F(ChangeStreamRewriteTest, CanInexactlyRewriteExprNorWithUnknownField) {
    // Note that rewrite of "$$ROOT" without a path is unsupported.
    auto spec = fromjson(
        "{$expr: {$and: [{$isNumber: '$clusterTime'},"
        "{$not: {$or: [{$isNumber: '$lsid'}, {$isArray: '$unknown'}]}}]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression =
        change_stream_rewrite::rewriteFilterForFields(getExpCtx(),
                                                      statusWithMatchExpression.getValue().get(),
                                                      bsonObjsArray,
                                                      {"clusterTime", "lsid"});

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        fromjson(
            "{$expr: {$and: [{$isNumber: ['$ts']}, {$not: [{$or: [{$isNumber: ['$lsid']}]}]}]}}"));
}

TEST_F(ChangeStreamRewriteTest, CannotInexactlyRewriteNegatedExprPredicate) {
    // Note that rewrite of "$$ROOT" without a path is unsupported.
    auto spec = fromjson("{$expr: {$not: {$and: [{$isNumber: '$lsid'}, {$isArray: '$$ROOT'}]}}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"lsid"});

    ASSERT(!rewrittenMatchExpression);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteExprLet) {
    auto spec = fromjson("{$expr: {$let: {vars: {v: '$clusterTime'}, in: {$isNumber: '$$v'}}}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression =
        change_stream_rewrite::rewriteFilterForFields(getExpCtx(),
                                                      statusWithMatchExpression.getValue().get(),
                                                      bsonObjsArray,
                                                      {"clusterTime", "lsid"});

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$expr: {$let: {vars: {v: '$ts'}, in: {$isNumber: ['$$v']}}}}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteExprFieldRefWithCurrentPrefix) {
    auto spec = fromjson("{$expr: {$isNumber: '$$CURRENT.clusterTime'}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"clusterTime"});

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, fromjson("{$expr: {$isNumber: ['$ts']}}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteExprFieldRefWithRootPrefix) {
    auto spec = fromjson("{$expr: {$isNumber: '$$ROOT.clusterTime'}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"clusterTime"});

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, fromjson("{$expr: {$isNumber: ['$ts']}}"));
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteExprFieldRefWithReassignedCurrentPrefix) {
    auto spec =
        fromjson("{$expr: {$let: {vars: {CURRENT: '$operationType'}, in: {$isNumber: '$lsid'}}}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression =
        change_stream_rewrite::rewriteFilterForFields(getExpCtx(),
                                                      statusWithMatchExpression.getValue().get(),
                                                      bsonObjsArray,
                                                      {"operationType", "lsid"});

    ASSERT(!rewrittenMatchExpression);
}

TEST_F(ChangeStreamRewriteTest, DoesNotRewriteUnrequestedFieldInExpr) {
    // The 'operationType' predicate could be rewritten but will be discarded in this test because
    // the 'operationType' field is not requested in the call to the 'rewriteFilterForFields()'
    // function.
    auto spec = fromjson(
        "{$expr: {$and: [{$eq: ['$operationType', 'insert']}, {$isNumber: '$clusterTime'}]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression =
        change_stream_rewrite::rewriteFilterForFields(getExpCtx(),
                                                      statusWithMatchExpression.getValue().get(),
                                                      bsonObjsArray,
                                                      {"clusterTime", "lsid"});

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, fromjson("{$expr: {$and: [{$isNumber: ['$ts']}]}}"));
}

//
// 'operationType' rewrites
//
TEST_F(ChangeStreamRewriteTest, CanRewriteEqPredicateOnOperationTypeWithInvalidOperandType) {
    auto query = BSON("operationType" << 1);
    auto statusWithMatchExpression = MatchExpressionParser::parse(query, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, fromjson("{$alwaysFalse: 1}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteEqPredicateOnOperationTypeWithUnknownOpType) {
    auto query = BSON("operationType" << "nonExisting");
    auto statusWithMatchExpression = MatchExpressionParser::parse(query, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, fromjson("{$alwaysFalse: 1}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteEqNullPredicateOnOperationType) {
    auto query = BSON("operationType" << BSONNULL);
    auto statusWithMatchExpression = MatchExpressionParser::parse(query, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, fromjson("{$alwaysFalse: 1}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteEqPredicateOnOperationTypeInsert) {
    auto spec = fromjson("{operationType: 'insert'}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, fromjson("{op: {$eq: 'i'}}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteEqPredicateOnOperationTypeReplace) {
    auto spec = fromjson("{operationType: 'replace'}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: [{op: {$eq: 'u'}}, {'o._id': {$exists: true}}]}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteEqPredicateOnOperationTypeDrop) {
    auto spec = fromjson("{operationType: 'drop'}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: [{op: {$eq: 'c'}}, {'o.drop': {$exists: true}}]}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteEqPredicateOnOperationTypeCreate) {
    auto spec = fromjson("{operationType: 'create'}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: [{op: {$eq: 'c'}}, {'o.create': {$exists: true}}]}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteEqPredicateOnOperationTypeRename) {
    auto spec = fromjson("{operationType: 'rename'}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        fromjson("{$and: [{op: {$eq: 'c'}}, {'o.renameCollection': {$exists: true}}]}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteEqPredicateOnOperationTypeDropDatabase) {
    auto spec = fromjson("{operationType: 'dropDatabase'}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: [{op: {$eq: 'c'}}, {'o.dropDatabase': {$exists: true}}]}"));
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteInequalityPredicateOnOperationType) {
    auto spec = fromjson("{operationType: {$gt: 'insert'}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"operationType"});
    ASSERT_FALSE(rewrittenMatchExpression);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteEqPredicateOnOperationTypeSubField) {
    auto spec = fromjson("{'operationType.subField': 'subOp'}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, fromjson("{$alwaysFalse: 1}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteEqNullPredicateOnOperationTypeSubField) {
    auto spec = fromjson("{'operationType.subField': {$eq: null}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, fromjson("{$alwaysTrue: 1}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteInPredicateOnOperationType) {
    auto expr = BSON("operationType" << BSON("$in" << BSON_ARRAY("drop" << "create"
                                                                        << "insert")));
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();

    ASSERT_BSONOBJ_EQ(
        MatchExpressionParser::parseAndNormalize(rewrittenPredicate, getExpCtx())->serialize(),
        MatchExpressionParser::parseAndNormalize(
            BSON(OR(fromjson("{$and: [{op: {$eq: 'c'}}, {'o.create': {$exists: true}}]}"),
                    fromjson("{$and: [{op: {$eq: 'c'}}, {'o.drop': {$exists: true}}]}"),
                    fromjson("{op: {$eq: 'i'}}"))),
            getExpCtx())
            ->serialize());
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteInPredicateWithRegexOnOperationType) {
    auto expr =
        BSON("operationType" << BSON("$in" << BSON_ARRAY(BSONRegEx("^in*sert") << "update")));
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"operationType"});
    ASSERT_FALSE(rewrittenMatchExpression);
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteRegexPredicateOnOperationType) {
    auto expr = BSON("operationType" << BSONRegEx("^in*sert"));
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"operationType"});
    ASSERT_FALSE(rewrittenMatchExpression);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteInPredicateOnOperationTypeWithInvalidOperandType) {
    auto expr = BSON("operationType" << BSON("$in" << BSON_ARRAY(1)));
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        MatchExpressionParser::parseAndNormalize(rewrittenPredicate, getExpCtx())->serialize(),
        MatchExpressionParser::parseAndNormalize(BSON(OR(fromjson("{$alwaysFalse: 1}"))),
                                                 getExpCtx())
            ->serialize());
}

TEST_F(ChangeStreamRewriteTest,
       CanRewriteInPredicateOnOperationTypeWithInvalidAndValidOperandTypes) {
    auto expr = BSON("operationType" << BSON("$in" << BSON_ARRAY(1 << "insert")));
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        MatchExpressionParser::parseAndNormalize(rewrittenPredicate, getExpCtx())->serialize(),
        MatchExpressionParser::parseAndNormalize(
            BSON(OR(fromjson("{$alwaysFalse: 1}"), fromjson("{op: {$eq: 'i'}}"))), getExpCtx())
            ->serialize());
}

TEST_F(ChangeStreamRewriteTest, CanRewriteInPredicateOnOperationTypeWithUnknownOpType) {
    auto expr = BSON("operationType" << BSON("$in" << BSON_ARRAY("unknown" << "insert")));
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        MatchExpressionParser::parseAndNormalize(rewrittenPredicate, getExpCtx())->serialize(),
        MatchExpressionParser::parseAndNormalize(
            BSON(OR(fromjson("{op: {$eq: 'i'}}"), fromjson("{$alwaysFalse: 1}"))), getExpCtx())
            ->serialize());
}

TEST_F(ChangeStreamRewriteTest, CanRewriteEmptyInPredicateOnOperationType) {
    auto spec = fromjson("{operationType: {$in: []}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, fromjson("{$alwaysFalse: 1}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNinPredicateOnOperationType) {
    auto expr = BSON("operationType" << BSON("$nin" << BSON_ARRAY("drop" << "insert")));
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        MatchExpressionParser::parseAndNormalize(rewrittenPredicate, getExpCtx())->serialize(),
        MatchExpressionParser::parseAndNormalize(
            BSON(NOR(BSON(OR(fromjson("{$and: [{op: {$eq: 'c'}}, {'o.drop': {$exists: true}}]}"),
                             fromjson("{op: {$eq: 'i'}}"))))),
            getExpCtx())
            ->serialize());
}

TEST_F(ChangeStreamRewriteTest, RewritesOnlyUpdateCorrectly) {
    auto expr = BSON("operationType" << BSON("$eq" << "update"));
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        MatchExpressionParser::parseAndNormalize(rewrittenPredicate, getExpCtx())->serialize(),
        MatchExpressionParser::parseAndNormalize(
            fromjson("{ $and: [ { op: { $eq: 'u' } }, { 'o._id': { $exists: false } } ] }"),
            getExpCtx())
            ->serialize());
}

TEST_F(ChangeStreamRewriteTest, RewritesOnlyReplaceCorrectly) {
    auto expr = BSON("operationType" << BSON("$eq" << "replace"));
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        MatchExpressionParser::parseAndNormalize(rewrittenPredicate, getExpCtx())->serialize(),
        MatchExpressionParser::parseAndNormalize(
            fromjson("{ $and: [ { op: { $eq: 'u' } }, { 'o._id': { $exists: true } } ] }"),
            getExpCtx())
            ->serialize());
}

TEST_F(ChangeStreamRewriteTest, RewritesUpdateAndReplaceFilterToSimpleUpdateFilter) {
    auto expr = BSON("operationType" << BSON("$in" << BSON_ARRAY("update" << "replace")));
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        MatchExpressionParser::parseAndNormalize(rewrittenPredicate, getExpCtx())->serialize(),
        MatchExpressionParser::parseAndNormalize(fromjson("{ op: { $eq: 'u' } }"), getExpCtx())
            ->serialize());
}

TEST_F(ChangeStreamRewriteTest, RewritesAllCrudOperationsToSimpleInLookup) {
    // This filter is rewritten nicely to just '{ op: { $in: [ "d", "i", "u" ] } }'.

    auto expr = BSON("operationType" << BSON("$in" << BSON_ARRAY("update" << "insert"
                                                                          << "delete"
                                                                          << "replace")));
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        MatchExpressionParser::parseAndNormalize(rewrittenPredicate, getExpCtx())->serialize(),
        MatchExpressionParser::parseAndNormalize(fromjson("{ op: { $in: [ 'd', 'i', 'u' ] } }"),
                                                 getExpCtx())
            ->serialize());
}

TEST_F(ChangeStreamRewriteTest, CanRewriteExprWithOperationType) {
    auto spec = fromjson("{$expr: {$eq: ['$operationType', 'insert']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"operationType"});

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();

    const std::string expectedRewrite = R"(
{
    $expr: {
        $eq: [
            {
                $switch: {
                    branches: [
                        {case: {$eq: ['$op', {$const: 'i'}]}, then: {$const: 'insert'}},
                        {
                            case: {
                                $and:
                                    [{$eq: ['$op', {$const: 'u'}]}, {$eq: ['$o._id', '$$REMOVE']}]
                            },
                            then: {$const: 'update'}
                        },
                        {
                            case: {
                                $and:
                                    [{$eq: ['$op', {$const: 'u'}]}, {$ne: ['$o._id', '$$REMOVE']}]
                            },
                            then: {$const: 'replace'}
                        },
                        {case: {$eq: ['$op', {$const: 'd'}]}, then: {$const: 'delete'}},
                        {case: {$ne: ['$op', {$const: 'c'}]}, then: '$$REMOVE'},
                        {case: {$ne: ['$o.drop', '$$REMOVE']}, then: {$const: 'drop'}},
                        {case: {$ne: ['$o.create', '$$REMOVE']}, then: {$const: 'create'}},
                        {
                            case: {$ne: ['$o.dropDatabase', '$$REMOVE']},
                            then: {$const: 'dropDatabase'}
                        },
                        {case: {$ne: ['$o.renameCollection', '$$REMOVE']}, then: {$const: 'rename'}},
                        {
                            case: {$ne: ['$o.createIndexes', '$$REMOVE']},
                            then: {$const: 'createIndexes'}
                        },
                        {
                            case: {$ne: ['$o.commitIndexBuild', '$$REMOVE']},
                            then: {$const: 'createIndexes'}
                        },
                        {
                            case: {$ne: ['$o.startIndexBuild', '$$REMOVE']},
                            then: {$const: 'startIndexBuild'}
                        },
                        {
                            case: {$ne: ['$o.abortIndexBuild', '$$REMOVE']},
                            then: {$const: 'abortIndexBuild'}
                        },
                        {
                            case: {$ne: ['$o.dropIndexes', '$$REMOVE']},
                            then: {$const: 'dropIndexes'}
                        },
                        {
                            case: {$ne: ['$o.collMod', '$$REMOVE']},
                            then: {$const: 'modify'}
                        }
                    ],
                    default: '$$REMOVE'
                }
            },
            {$const: 'insert'}
        ]
    }
})";

    ASSERT_BSONOBJ_EQ(rewrittenPredicate, fromjson(expectedRewrite));
}

//
// 'documentKey' rewrites
//
TEST_F(ChangeStreamRewriteTest, CanRewriteEqPredicateOnFieldDocumentKey) {
    auto spec = fromjson("{documentKey: {_id: 'bar', foo: 'baz'}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"documentKey"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$or: ["
                               "  {$and: ["
                               "    {op: {$eq: 'u'}},"
                               "    {o2: {$eq: {_id: 'bar', foo: 'baz'}}}"
                               "  ]},"
                               "  {$and: ["
                               "    {op: {$eq: 'd'}},"
                               "    {o: {$eq: {_id: 'bar', foo: 'baz'}}}"
                               "  ]},"
                               "  {$and: ["
                               "    {op: {$eq: 'i'}},"
                               "    {o2: {$eq: {_id: 'bar', foo: 'baz'}}}"
                               "  ]}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteEqNullPredicateOnFieldDocumentKey) {
    auto spec = fromjson("{documentKey: {$eq: null}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"documentKey"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$or: ["
                               "  {$nor: ["
                               "    {op: {$eq: 'i'}},"
                               "    {op: {$eq: 'u'}},"
                               "    {op: {$eq: 'd'}}"
                               "  ]},"
                               "  {$and: ["
                               "    {op: {$eq: 'u'}},"
                               "    {o2: {$eq: null}}"
                               "  ]},"
                               "  {$and: ["
                               "    {op: {$eq: 'd'}},"
                               "    {o: {$eq: null}}"
                               "  ]},"
                               "  {$and: ["
                               "    {op: {$eq: 'i'}},"
                               "    {o2: {$eq: null}}"
                               "  ]}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteInPredicateOnFieldDocumentKey) {
    auto spec = fromjson("{documentKey: {$in: [{}, {_id: 'bar'}, {_id: 'bar', foo: 'baz'}]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"documentKey"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$or: ["
                               "  {$and: ["
                               "    {op: {$eq: 'u'}},"
                               "    {o2: {$in: [{}, {_id: 'bar'}, {_id: 'bar', foo: 'baz'}]}}"
                               "  ]},"
                               "  {$and: ["
                               "    {op: {$eq: 'd'}},"
                               "    {o: {$in: [{}, {_id: 'bar'}, {_id: 'bar', foo: 'baz'}]}}"
                               "  ]},"
                               "  {$and: ["
                               "    {op: {$eq: 'i'}},"
                               "    {o2: {$in: [{}, {_id: 'bar'}, {_id: 'bar', foo: 'baz'}]}}"
                               "  ]}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteArbitraryPredicateOnFieldDocumentKey) {
    auto spec = fromjson("{documentKey: {$gt: 'bar'}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"documentKey"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$or: ["
                               "  {$and: ["
                               "    {op: {$eq: 'u'}},"
                               "    {o2: {$gt: 'bar'}}"
                               "  ]},"
                               "  {$and: ["
                               "    {op: {$eq: 'd'}},"
                               "    {o: {$gt: 'bar'}}"
                               "  ]},"
                               "  {$and: ["
                               "    {op: {$eq: 'i'}},"
                               "    {o2: {$gt: 'bar'}}"
                               "  ]}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CanExactlyRewriteExprPredicateOnFieldDocumentKey) {
    auto spec = fromjson("{$expr: {$lt: ['$documentKey', 'bar']}}");

    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"documentKey"});

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        fromjson("{$expr:"
                 "  {$lt: ["
                 "    {$switch: {"
                 "      branches: ["
                 "        {case: {$eq: ['$op', {$const: 'd'}]}, then: '$o'},"
                 "        {case: {$in: ['$op', [{$const: 'i'}, {$const: 'u'}]]}, then: '$o2'}"
                 "      ],"
                 "      default: '$$REMOVE'"
                 "    }},"
                 "    {$const: 'bar'}"
                 "  ]}"
                 "}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteArbitraryPredicateOnFieldDocumentKeyId) {
    auto spec = fromjson("{'documentKey._id': {$lt: 'bar'}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"documentKey"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$or: ["
                               "  {$and: ["
                               "    {op: {$eq: 'u'}},"
                               "    {'o2._id': {$lt: 'bar'}}"
                               "  ]},"
                               "  {$and: ["
                               "    {op: {$eq: 'd'}},"
                               "    {'o._id': {$lt: 'bar'}}"
                               "  ]},"
                               "  {$and: ["
                               "    {op: {$eq: 'i'}},"
                               "    {'o2._id': {$lt: 'bar'}}"
                               "  ]}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteEqNullPredicateOnFieldDocumentKeyId) {
    auto spec = fromjson("{'documentKey._id': {$eq: null}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"documentKey"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$or: ["
                               "  {$nor: ["
                               "    {op: {$eq: 'i'}},"
                               "    {op: {$eq: 'u'}},"
                               "    {op: {$eq: 'd'}}"
                               "  ]},"
                               "  {$and: ["
                               "    {op: {$eq: 'u'}},"
                               "    {'o2._id': {$eq: null}}"
                               "  ]},"
                               "  {$and: ["
                               "    {op: {$eq: 'd'}},"
                               "    {'o._id': {$eq: null}}"
                               "  ]},"
                               "  {$and: ["
                               "    {op: {$eq: 'i'}},"
                               "    {'o2._id': {$eq: null}}"
                               "  ]}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CanExactlyRewriteExprPredicateOnFieldDocumentKeyId) {
    auto spec = fromjson("{$expr: {$lt: ['$documentKey._id', 'bar']}}");

    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"documentKey"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        fromjson("{$expr:"
                 "  {$lt: ["
                 "    {$switch: {"
                 "      branches: ["
                 "        {case: {$eq: ['$op', {$const: 'd'}]}, then: '$o._id'},"
                 "        {case: {$in: ['$op', [{$const: 'i'}, {$const: 'u'}]]}, then: '$o2._id'}"
                 "      ],"
                 "      default: '$$REMOVE'"
                 "    }},"
                 "    {$const: 'bar'}"
                 "  ]}"
                 "}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteArbitraryPredicateOnFieldDocumentKeyFoo) {
    auto spec = fromjson("{'documentKey.foo': {$gt: 'bar'}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"documentKey"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$or: ["
                               "  {$and: ["
                               "    {op: {$eq: 'u'}},"
                               "    {'o2.foo': {$gt: 'bar'}}"
                               "  ]},"
                               "  {$and: ["
                               "    {op: {$eq: 'd'}},"
                               "    {'o.foo': {$gt: 'bar'}}"
                               "  ]},"
                               "  {$and: ["
                               "    {op: {$eq: 'i'}},"
                               "    {'o2.foo': {$gt: 'bar'}}"
                               "  ]}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteEqNullPredicateOnFieldDocumentKeyFoo) {
    auto spec = fromjson("{'documentKey.foo': {$eq: null}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"documentKey"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$or: ["
                               "  {$nor: ["
                               "    {op: {$eq: 'i'}},"
                               "    {op: {$eq: 'u'}},"
                               "    {op: {$eq: 'd'}}"
                               "  ]},"
                               "  {$and: ["
                               "    {op: {$eq: 'u'}},"
                               "    {'o2.foo': {$eq: null}}"
                               "  ]},"
                               "  {$and: ["
                               "    {op: {$eq: 'd'}},"
                               "    {'o.foo': {$eq: null}}"
                               "  ]},"
                               "  {$and: ["
                               "    {op: {$eq: 'i'}},"
                               "    {'o2.foo': {$eq: null}}"
                               "  ]}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CanExactlyRewritePredicateOnFieldDocumentKey) {
    auto spec = fromjson("{documentKey: {$not: {$eq: {_id: 'bar'}}}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"documentKey"});

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$nor: ["
                               "  {$or: ["
                               "    {$and: ["
                               "      {op: {$eq: 'u'}},"
                               "      {o2: {$eq: {_id: 'bar'}}}"
                               "    ]},"
                               "    {$and: ["
                               "      {op: {$eq: 'd'}},"
                               "      {o: {$eq: {_id: 'bar'}}}"
                               "    ]},"
                               "    {$and: ["
                               "      {op: {$eq: 'i'}},"
                               "      {o2: {$eq: {_id: 'bar'}}}"
                               "    ]}"
                               "  ]}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CanExactlyRewritePredicateOnFieldDocumentKeyId) {
    auto spec = fromjson("{'documentKey._id': {$not: {$eq: 'bar'}}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"documentKey"});
    ASSERT(rewrittenMatchExpression);

    // Every documentKey must have an _id field, so a predicate on this field can always be exactly
    // rewritten.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$nor: ["
                               "  {$or: ["
                               "    {$and: ["
                               "      {op: {$eq: 'u'}},"
                               "      {'o2._id': {$eq: 'bar'}}"
                               "    ]},"
                               "    {$and: ["
                               "      {op: {$eq: 'd'}},"
                               "      {'o._id': {$eq: 'bar'}}"
                               "    ]},"
                               "    {$and: ["
                               "      {op: {$eq: 'i'}},"
                               "      {'o2._id': {$eq: 'bar'}}"
                               "    ]}"
                               "  ]}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CanExactlyRewritePredicateOnFieldDocumentKeyFoo) {
    auto spec = fromjson("{'documentKey.foo': {$not: {$eq: 'bar'}}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"documentKey"});

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$nor: ["
                               "  {$or: ["
                               "    {$and: ["
                               "      {op: {$eq: 'u'}},"
                               "      {'o2.foo': {$eq: 'bar'}}"
                               "    ]},"
                               "    {$and: ["
                               "      {op: {$eq: 'd'}},"
                               "      {'o.foo': {$eq: 'bar'}}"
                               "    ]},"
                               "    {$and: ["
                               "      {op: {$eq: 'i'}},"
                               "      {'o2.foo': {$eq: 'bar'}}"
                               "    ]}"
                               "  ]}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteExprPredicateOnFieldDocumentKeyFoo) {
    auto spec = fromjson("{$expr: {$or: ['$documentKey.foo']}}");

    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"documentKey"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        fromjson("{$expr:"
                 "  {$or: ["
                 "    {$switch: {"
                 "      branches: ["
                 "        {case: {$eq: ['$op', {$const: 'd'}]}, then: '$o.foo'},"
                 "        {case: {$in: ['$op', [{$const: 'i'}, {$const: 'u'}]]}, then: '$o2.foo'}"
                 "      ],"
                 "      default: '$$REMOVE'"
                 "    }}"
                 "  ]}"
                 "}"));
}

TEST_F(ChangeStreamRewriteTest, CanExactlyRewriteExprPredicateOnFieldDocumentKeyFoo) {
    auto spec = fromjson("{$expr: {$lt: ['$documentKey.foo', 'bar']}}");

    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"documentKey"});

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        fromjson("{$expr:"
                 "  {$lt: ["
                 "    {$switch: {"
                 "      branches: ["
                 "        {case: {$eq: ['$op', {$const: 'd'}]}, then: '$o.foo'},"
                 "        {case: {$in: ['$op', [{$const: 'i'}, {$const: 'u'}]]}, then: '$o2.foo'}"
                 "      ],"
                 "      default: '$$REMOVE'"
                 "    }},"
                 "    {$const: 'bar'}"
                 "  ]}"
                 "}"));
}

//
// 'fullDocument' rewrites
//
TEST_F(ChangeStreamRewriteTest, CanRewriteArbitraryPredicateOnFieldFullDocumentFoo) {
    auto spec = fromjson("{'fullDocument.foo': {$lt: 'bar'}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"fullDocument"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$or: ["
                               "  {$and: ["
                               "    {op: {$eq: 'u'}},"
                               "    {'o._id': {$not: {$exists: true}}}"
                               "  ]},"
                               "  {$and: ["
                               "    {$or: ["
                               "      {$and: ["
                               "        {op: {$eq: 'u'}},"
                               "        {'o._id': {$exists: true}}"
                               "      ]},"
                               "      {op: {$eq: 'i'}}"
                               "    ]},"
                               "    {'o.foo': {$lt: 'bar'}}"
                               "  ]}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNullComparisonPredicateOnFieldFullDocumentFoo) {
    auto spec = fromjson("{'fullDocument.foo': {$eq: null}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"fullDocument"});
    ASSERT(rewrittenMatchExpression);

    // Note that the filter below includes a predicate on delete and non-CRUD events. These are only
    // present when the user's predicate matches a non-existent field. This is because these change
    // events never have a 'fullDocument' field, and so we either match all such events in the oplog
    // or none of them, depending on how the predicate evaluates against a missing field.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$or: ["
                               "  {$and: ["
                               "    {op: {$eq: 'u'}},"
                               "    {'o._id': {$not: {$exists: true}}}"
                               "  ]},"
                               "  {$and: ["
                               "    {$or: ["
                               "      {$and: ["
                               "        {op: {$eq: 'u'}},"
                               "        {'o._id': {$exists: true}}"
                               "      ]},"
                               "      {op: {$eq: 'i'}}"
                               "    ]},"
                               "    {'o.foo': {$eq: null}}"
                               "  ]},"
                               "  {op: {$eq: 'd'}},"
                               "  {$nor: [{op: {$eq: 'i'}}, {op: {$eq: 'u'}}, {op: {$eq: 'd'}}]}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CannotExactlyRewritePredicateOnFieldFullDocumentFoo) {
    auto spec = fromjson("{'fullDocument.foo': {$not: {$eq: 'bar'}}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"fullDocument"});

    // Because the 'fullDocument' field can be populated later in the pipeline for update events
    // (via the '{fullDocument: "updateLookup"}' option), it's impractical to try to generate a
    // rewritten predicate that matches exactly.
    ASSERT(rewrittenMatchExpression == nullptr);
}

//
// 'fullDocumentBeforeChange' rewrites
//
TEST_F(ChangeStreamRewriteTest, CannotRewriteNullPredicateOnFieldFullDocumentBeforeChange) {
    auto spec = fromjson("{'fullDocumentBeforeChange': null}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression =
        change_stream_rewrite::rewriteFilterForFields(getExpCtx(),
                                                      statusWithMatchExpression.getValue().get(),
                                                      bsonObjsArray,
                                                      {"fullDocumentBeforeChange"});

    // '{fullDocumentBeforeChange: null}' cannot be rewritten
    ASSERT(rewrittenMatchExpression == nullptr);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNonNullPredicateOnFieldFullDocumentBeforeChange) {
    auto spec = fromjson("{'fullDocumentBeforeChange': 1}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression =
        change_stream_rewrite::rewriteFilterForFields(getExpCtx(),
                                                      statusWithMatchExpression.getValue().get(),
                                                      bsonObjsArray,
                                                      {"fullDocumentBeforeChange"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$or:[{$and:[{op:{$eq:'u'}}]}, {$and:[{op:{$eq:'d'}}]}]}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNonNullPredicateOnFieldFullDocumentBeforeChangeFoo) {
    auto spec = fromjson("{'fullDocumentBeforeChange.foo': {$eq: 'bar'}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression =
        change_stream_rewrite::rewriteFilterForFields(getExpCtx(),
                                                      statusWithMatchExpression.getValue().get(),
                                                      bsonObjsArray,
                                                      {"fullDocumentBeforeChange"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$or:[{$and:[{op:{$eq:'u'}}]}, {$and:[{op:{$eq:'d'}}]}]}"));
}

TEST_F(ChangeStreamRewriteTest, CannotExactlyRewritePredicateOnFieldFullDocumentBeforeChangeFoo) {
    auto spec = fromjson("{'fullDocumentBeforeChange.foo': {$not: {$eq: 'bar'}}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression =
        change_stream_rewrite::rewriteFilterForFields(getExpCtx(),
                                                      statusWithMatchExpression.getValue().get(),
                                                      bsonObjsArray,
                                                      {"fullDocumentBeforeChange"});

    ASSERT(rewrittenMatchExpression == nullptr);
}

TEST_F(ChangeStreamRewriteTest, CannotExactlyRewritePredicateOnFieldFullDocumentBeforeChangeId) {
    auto spec = fromjson("{'fullDocumentBeforeChange._id': {$not: {$lt: 3}}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression =
        change_stream_rewrite::rewriteFilterForFields(getExpCtx(),
                                                      statusWithMatchExpression.getValue().get(),
                                                      bsonObjsArray,
                                                      {"fullDocumentBeforeChange"});

    ASSERT(rewrittenMatchExpression == nullptr);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNonNullPredicateOnFieldFullDocumentBeforeChangeId) {
    auto spec = fromjson("{'fullDocumentBeforeChange._id': {$lt: 3}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression =
        change_stream_rewrite::rewriteFilterForFields(getExpCtx(),
                                                      statusWithMatchExpression.getValue().get(),
                                                      bsonObjsArray,
                                                      {"fullDocumentBeforeChange"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$or:["
                               "   {$and: ["
                               "       {op: {$eq: 'u'}},"
                               "       {'o2._id': {$lt: 3}}"
                               "   ]}, "
                               "   {$and: ["
                               "       {op: {$eq: 'd'}},"
                               "       {'o._id': {$lt: 3}}"
                               "   ]}"
                               "]}"));
}

//
// 'ns' rewrites
//
TEST_F(ChangeStreamRewriteTest, CanRewriteFullNamespaceObject) {
    auto expCtx = getExpCtx();
    auto query = BSON("ns" << BSON("db" << expCtx->getNamespaceString().db_forTest() << "coll"
                                        << expCtx->getNamespaceString().coll()));
    auto statusWithMatchExpression = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    const std::string ns = std::string{expCtx->getNamespaceString().db_forTest()} + "." +
        std::string{expCtx->getNamespaceString().coll()};
    const std::string cmdNs = std::string{expCtx->getNamespaceString().db_forTest()} + ".$cmd";

    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(
            BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"), BSON("ns" << BSON("$eq" << ns)))),
            BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                     BSON(OR(BSON("o.renameCollection" << BSON("$eq" << ns)),
                             BSON(AND(BSON("ns" << BSON("$eq" << cmdNs)),
                                      BSON("o.drop"
                                           << BSON("$eq" << expCtx->getNamespaceString().coll())))),
                             BSON(AND(BSON("ns" << BSON("$eq" << cmdNs)),
                                      BSON("o.create"
                                           << BSON("$eq" << expCtx->getNamespaceString().coll())))),
                             BSON(AND(BSON("ns" << BSON("$eq" << cmdNs)),
                                      BSON("o.createIndexes"
                                           << BSON("$eq" << expCtx->getNamespaceString().coll())))),
                             BSON(AND(BSON("ns" << BSON("$eq" << cmdNs)),
                                      BSON("o.commitIndexBuild"
                                           << BSON("$eq" << expCtx->getNamespaceString().coll())))),
                             BSON(AND(BSON("ns" << BSON("$eq" << cmdNs)),
                                      BSON("o.startIndexBuild"
                                           << BSON("$eq" << expCtx->getNamespaceString().coll())))),
                             BSON(AND(BSON("ns" << BSON("$eq" << cmdNs)),
                                      BSON("o.abortIndexBuild"
                                           << BSON("$eq" << expCtx->getNamespaceString().coll())))),
                             BSON(AND(BSON("ns" << BSON("$eq" << cmdNs)),
                                      BSON("o.dropIndexes"
                                           << BSON("$eq" << expCtx->getNamespaceString().coll())))),
                             BSON(AND(BSON("ns" << BSON("$eq" << cmdNs)),
                                      BSON("o.collMod"
                                           << BSON("$eq" << expCtx->getNamespaceString().coll())))),
                             BSON(AND(fromjson("{$alwaysFalse: 1}"),
                                      fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceObjectWithSwappedField) {
    auto expCtx = getExpCtx();
    auto query = BSON("ns" << BSON("coll" << expCtx->getNamespaceString().coll() << "db"
                                          << expCtx->getNamespaceString().db_forTest()));
    auto statusWithMatchExpression = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"), fromjson("{$alwaysFalse: 1 }"))),
                BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                         BSON(OR(fromjson("{$alwaysFalse: 1 }"),  // drop.
                                 fromjson("{$alwaysFalse: 1 }"),  // rename.
                                 fromjson("{$alwaysFalse: 1 }"),  // create.
                                 fromjson("{$alwaysFalse: 1 }"),  // createIndexes.
                                 fromjson("{$alwaysFalse: 1 }"),  // commitIndexBuild.
                                 fromjson("{$alwaysFalse: 1 }"),  // startIndexBuild.
                                 fromjson("{$alwaysFalse: 1 }"),  // abortIndexBuild.
                                 fromjson("{$alwaysFalse: 1 }"),  // dropIndexes.
                                 fromjson("{$alwaysFalse: 1 }"),  // collMod.
                                 BSON(AND(fromjson("{$alwaysFalse: 1}"),
                                          fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceObjectWithOnlyDbField) {
    auto expCtx = getExpCtx();
    auto query = BSON("ns" << BSON("db" << expCtx->getNamespaceString().db_forTest()));
    auto statusWithMatchExpression = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT(rewrittenMatchExpression);

    const std::string cmdNs = std::string{expCtx->getNamespaceString().db_forTest()} + ".$cmd";

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"), fromjson("{$alwaysFalse: 1}"))),
                BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                         BSON(OR(fromjson("{$alwaysFalse: 1 }"),  // drop.
                                 fromjson("{$alwaysFalse: 1 }"),  // rename.
                                 fromjson("{$alwaysFalse: 1 }"),  // create.
                                 fromjson("{$alwaysFalse: 1 }"),  // createIndexes.
                                 fromjson("{$alwaysFalse: 1 }"),  // commitIndexBuild.
                                 fromjson("{$alwaysFalse: 1 }"),  // startIndexBuild.
                                 fromjson("{$alwaysFalse: 1 }"),  // abortIndexBuild.
                                 fromjson("{$alwaysFalse: 1 }"),  // dropIndexes.
                                 fromjson("{$alwaysFalse: 1 }"),  // collMod.
                                 BSON(AND(BSON(AND(BSON("ns" << BSON("$eq" << cmdNs)))),
                                          fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceObjectWithOnlyCollectionField) {
    auto expCtx = getExpCtx();
    auto query = BSON("ns" << BSON("coll" << expCtx->getNamespaceString().coll()));
    auto statusWithMatchExpression = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"), fromjson("{$alwaysFalse: 1 }"))),
                BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                         BSON(OR(fromjson("{$alwaysFalse: 1 }"),  // drop.
                                 fromjson("{$alwaysFalse: 1 }"),  // rename.
                                 fromjson("{$alwaysFalse: 1 }"),  // create.
                                 fromjson("{$alwaysFalse: 1 }"),  // createIndexes.
                                 fromjson("{$alwaysFalse: 1 }"),  // commitIndexBuild.
                                 fromjson("{$alwaysFalse: 1 }"),  // startIndexBuild.
                                 fromjson("{$alwaysFalse: 1 }"),  // abortIndexBuild.
                                 fromjson("{$alwaysFalse: 1 }"),  // dropIndexes.
                                 fromjson("{$alwaysFalse: 1 }"),  // collMod.
                                 BSON(AND(fromjson("{$alwaysFalse: 1}"),
                                          fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceObjectWithInvalidDbField) {
    auto expCtx = getExpCtx();
    auto query = BSON("ns" << BSON("db" << 1 << "coll" << expCtx->getNamespaceString().coll()));
    auto statusWithMatchExpression = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"), fromjson("{$alwaysFalse: 1 }"))),
                BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                         BSON(OR(fromjson("{$alwaysFalse: 1 }"),  // drop.
                                 fromjson("{$alwaysFalse: 1 }"),  // rename.
                                 fromjson("{$alwaysFalse: 1 }"),  // create.
                                 fromjson("{$alwaysFalse: 1 }"),  // createIndexes.
                                 fromjson("{$alwaysFalse: 1 }"),  // commitIndexBuild.
                                 fromjson("{$alwaysFalse: 1 }"),  // startIndexBuild.
                                 fromjson("{$alwaysFalse: 1 }"),  // abortIndexBuild.
                                 fromjson("{$alwaysFalse: 1 }"),  // dropIndexes.
                                 fromjson("{$alwaysFalse: 1 }"),  // collMod.
                                 BSON(AND(fromjson("{$alwaysFalse: 1}"),
                                          fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceObjectWithInvalidCollField) {
    auto expCtx = getExpCtx();
    auto query =
        BSON("ns" << BSON("db" << expCtx->getNamespaceString().db_forTest() << "coll" << 1));
    auto statusWithMatchExpression = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"), fromjson("{$alwaysFalse: 1 }"))),
                BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                         BSON(OR(fromjson("{$alwaysFalse: 1 }"),  // drop.
                                 fromjson("{$alwaysFalse: 1 }"),  // rename.
                                 fromjson("{$alwaysFalse: 1 }"),  // create.
                                 fromjson("{$alwaysFalse: 1 }"),  // createIndexes.
                                 fromjson("{$alwaysFalse: 1 }"),  // commitIndexBuild.
                                 fromjson("{$alwaysFalse: 1 }"),  // startIndexBuild.
                                 fromjson("{$alwaysFalse: 1 }"),  // abortIndexBuild.
                                 fromjson("{$alwaysFalse: 1 }"),  // dropIndexes.
                                 fromjson("{$alwaysFalse: 1 }"),  // collMod.
                                 BSON(AND(fromjson("{$alwaysFalse: 1}"),
                                          fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceObjectWithExtraField) {
    auto expCtx = getExpCtx();
    auto query = fromjson("{ns: {db: 'db', coll: 'coll', extra: 'extra'}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"), fromjson("{$alwaysFalse: 1 }"))),
                BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                         BSON(OR(fromjson("{$alwaysFalse: 1 }"),  // drop.
                                 fromjson("{$alwaysFalse: 1 }"),  // rename.
                                 fromjson("{$alwaysFalse: 1 }"),  // create.
                                 fromjson("{$alwaysFalse: 1 }"),  // createIndexes.
                                 fromjson("{$alwaysFalse: 1 }"),  // commitIndexBuild.
                                 fromjson("{$alwaysFalse: 1 }"),  // startIndexBuild.
                                 fromjson("{$alwaysFalse: 1 }"),  // abortIndexBuild.
                                 fromjson("{$alwaysFalse: 1 }"),  // dropIndexes.
                                 fromjson("{$alwaysFalse: 1 }"),  // collMod.
                                 BSON(AND(fromjson("{$alwaysFalse: 1}"),
                                          fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithStringDbFieldPath) {
    auto expCtx = getExpCtx();
    auto query = BSON("ns.db" << expCtx->getNamespaceString().db_forTest());
    auto statusWithMatchExpression = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    const std::string regexNs = "^" + std::string{expCtx->getNamespaceString().db_forTest()} +
        "\\." + std::string{DocumentSourceChangeStream::kRegexAllCollections};
    const std::string cmdNs = std::string{expCtx->getNamespaceString().db_forTest()} + ".$cmd";

    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"),
                         BSON("ns" << BSON("$regex" << regexNs)))),
                BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                         BSON(OR(BSON("o.renameCollection" << BSON("$regex" << regexNs)),
                                 BSON("ns" << BSON("$eq" << cmdNs)),  // drop.
                                 BSON("ns" << BSON("$eq" << cmdNs)),  // create.
                                 BSON("ns" << BSON("$eq" << cmdNs)),  // createIndex.
                                 BSON("ns" << BSON("$eq" << cmdNs)),  // commitIndexBuild.
                                 BSON("ns" << BSON("$eq" << cmdNs)),  // startIndexBuild.
                                 BSON("ns" << BSON("$eq" << cmdNs)),  // abortIndexBuild.
                                 BSON("ns" << BSON("$eq" << cmdNs)),  // dropIndex.
                                 BSON("ns" << BSON("$eq" << cmdNs)),  // collMod.
                                 BSON(AND(BSON("ns" << BSON("$eq" << cmdNs)),
                                          fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithCollectionFieldPath) {
    auto expCtx = getExpCtx();
    auto query = BSON("ns.coll" << expCtx->getNamespaceString().coll());
    auto statusWithMatchExpression = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    const std::string regexNs = std::string{DocumentSourceChangeStream::kRegexAllDBs} + "\\." +
        std::string{expCtx->getNamespaceString().coll()} + "$";

    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(
            BSON(
                AND(fromjson("{op: {$not: {$eq: 'c'}}}"), BSON("ns" << BSON("$regex" << regexNs)))),
            BSON(AND(
                fromjson("{op: {$eq: 'c'}}"),
                BSON(OR(
                    BSON("o.renameCollection" << BSON("$regex" << regexNs)),
                    BSON("o.drop" << BSON("$eq" << expCtx->getNamespaceString().coll())),
                    BSON("o.create" << BSON("$eq" << expCtx->getNamespaceString().coll())),
                    BSON("o.createIndexes" << BSON("$eq" << expCtx->getNamespaceString().coll())),
                    BSON("o.commitIndexBuild"
                         << BSON("$eq" << expCtx->getNamespaceString().coll())),
                    BSON("o.startIndexBuild" << BSON("$eq" << expCtx->getNamespaceString().coll())),
                    BSON("o.abortIndexBuild" << BSON("$eq" << expCtx->getNamespaceString().coll())),
                    BSON("o.dropIndexes" << BSON("$eq" << expCtx->getNamespaceString().coll())),
                    BSON("o.collMod" << BSON("$eq" << expCtx->getNamespaceString().coll())),
                    BSON(AND(fromjson("{$alwaysFalse: 1}"),
                             fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithRegexDbFieldPath) {
    auto expCtx = getExpCtx();
    auto query = BSON("ns.db" << BSONRegEx(R"(^unit.*$)"));
    auto statusWithMatchExpression = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(
            BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"),
                     fromjson(getNsDbRegexMatchExpr("$ns", R"(^unit.*$)")))),
            BSON(AND(
                fromjson("{op: {$eq: 'c'}}"),
                BSON(OR(fromjson(getNsDbRegexMatchExpr("$o.renameCollection", R"(^unit.*$)")),
                        fromjson(getNsDbRegexMatchExpr("$ns", R"(^unit.*$)")),  // drop.
                        fromjson(getNsDbRegexMatchExpr("$ns", R"(^unit.*$)")),  // create.
                        fromjson(getNsDbRegexMatchExpr("$ns", R"(^unit.*$)")),  // createIndexes.
                        fromjson(getNsDbRegexMatchExpr("$ns", R"(^unit.*$)")),  // commitIndexBuild.
                        fromjson(getNsDbRegexMatchExpr("$ns", R"(^unit.*$)")),  // startIndexBuild.
                        fromjson(getNsDbRegexMatchExpr("$ns", R"(^unit.*$)")),  // abortIndexBuild.
                        fromjson(getNsDbRegexMatchExpr("$ns", R"(^unit.*$)")),  // dropIndexes.
                        fromjson(getNsDbRegexMatchExpr("$ns", R"(^unit.*$)")),  // collMod.
                        BSON(AND(fromjson(getNsDbRegexMatchExpr("$ns", R"(^unit.*$)")),
                                 fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithRegexCollectionFieldPath) {
    auto expCtx = getExpCtx();
    auto query = BSON("ns.coll" << BSONRegEx(R"(^pipeline.*$)"));
    auto statusWithMatchExpression = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(
            BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"),
                     fromjson(getNsCollRegexMatchExpr("$ns", R"(^pipeline.*$)")))),
            BSON(AND(
                fromjson("{op: {$eq: 'c'}}"),
                BSON(OR(fromjson(getNsCollRegexMatchExpr("$o.renameCollection", R"(^pipeline.*$)")),
                        fromjson(getNsCollRegexMatchExpr("$o.drop", R"(^pipeline.*$)")),
                        fromjson(getNsCollRegexMatchExpr("$o.create", R"(^pipeline.*$)")),
                        fromjson(getNsCollRegexMatchExpr("$o.createIndexes", R"(^pipeline.*$)")),
                        fromjson(getNsCollRegexMatchExpr("$o.commitIndexBuild", R"(^pipeline.*$)")),
                        fromjson(getNsCollRegexMatchExpr("$o.startIndexBuild", R"(^pipeline.*$)")),
                        fromjson(getNsCollRegexMatchExpr("$o.abortIndexBuild", R"(^pipeline.*$)")),
                        fromjson(getNsCollRegexMatchExpr("$o.dropIndexes", R"(^pipeline.*$)")),
                        fromjson(getNsCollRegexMatchExpr("$o.collMod", R"(^pipeline.*$)")),
                        BSON(AND(fromjson("{ $alwaysFalse: 1 }"),
                                 fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteNamespaceWithInvalidDbFieldPath) {
    auto expCtx = getExpCtx();
    auto query = BSON("ns.db" << 1);
    auto statusWithMatchExpression = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT_FALSE(rewrittenMatchExpression);
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteNamespaceWithInvalidCollectionFieldPath) {
    auto expCtx = getExpCtx();
    auto query = BSON("ns.coll" << 1);
    auto statusWithMatchExpression = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT_FALSE(rewrittenMatchExpression);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithExtraDbFieldPath) {
    auto expCtx = getExpCtx();
    auto query = BSON("ns.db.subField" << "subDb");
    auto statusWithMatchExpression = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"), fromjson("{$alwaysFalse: 1 }"))),
                BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                         BSON(OR(fromjson("{$alwaysFalse: 1 }"),  // drop
                                 fromjson("{$alwaysFalse: 1 }"),  // rename
                                 fromjson("{$alwaysFalse: 1 }"),  // create
                                 fromjson("{$alwaysFalse: 1 }"),  // createIndexes
                                 fromjson("{$alwaysFalse: 1 }"),  // commitIndexBuild
                                 fromjson("{$alwaysFalse: 1 }"),  // startIndexBuild
                                 fromjson("{$alwaysFalse: 1 }"),  // abortIndexBuild
                                 fromjson("{$alwaysFalse: 1 }"),  // dropIndexes
                                 fromjson("{$alwaysFalse: 1 }"),  // collMod
                                 BSON(AND(fromjson("{$alwaysFalse: 1 }"),
                                          fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithExtraCollectionFieldPath) {
    auto expCtx = getExpCtx();
    auto query = BSON("ns.coll.subField" << "subColl");
    auto statusWithMatchExpression = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"), fromjson("{$alwaysFalse: 1 }"))),
                BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                         BSON(OR(fromjson("{$alwaysFalse: 1 }"),  // drop
                                 fromjson("{$alwaysFalse: 1 }"),  // rename
                                 fromjson("{$alwaysFalse: 1 }"),  // create
                                 fromjson("{$alwaysFalse: 1 }"),  // createIndexes
                                 fromjson("{$alwaysFalse: 1 }"),  // commitIndexBuild
                                 fromjson("{$alwaysFalse: 1 }"),  // startIndexBuild
                                 fromjson("{$alwaysFalse: 1 }"),  // abortIndexBuild
                                 fromjson("{$alwaysFalse: 1 }"),  // dropIndexes
                                 fromjson("{$alwaysFalse: 1 }"),  // collMod
                                 BSON(AND(fromjson("{$alwaysFalse: 1 }"),
                                          fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithInvalidFieldPath) {
    auto expCtx = getExpCtx();
    auto query = BSON("ns.unknown" << "test");
    auto statusWithMatchExpression = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"), fromjson("{$alwaysFalse: 1 }"))),
                BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                         BSON(OR(fromjson("{$alwaysFalse: 1 }"),  // drop
                                 fromjson("{$alwaysFalse: 1 }"),  // rename
                                 fromjson("{$alwaysFalse: 1 }"),  // create
                                 fromjson("{$alwaysFalse: 1 }"),  // createIndexes
                                 fromjson("{$alwaysFalse: 1 }"),  // commitIndexBuild
                                 fromjson("{$alwaysFalse: 1 }"),  // startIndexBuild
                                 fromjson("{$alwaysFalse: 1 }"),  // abortIndexBuild
                                 fromjson("{$alwaysFalse: 1 }"),  // dropIndexes
                                 fromjson("{$alwaysFalse: 1 }"),  // collMod
                                 BSON(AND(fromjson("{$alwaysFalse: 1 }"),
                                          fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithInExpressionOnDb) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'ns.db': {$in: ['test', 'news']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT(rewrittenMatchExpression);

    const std::string firstRegexNs = std::string("^") + "news" + "\\." +
        std::string{DocumentSourceChangeStream::kRegexAllCollections};
    const std::string secondRegexNs = std::string("^") + "test" + "\\." +
        std::string{DocumentSourceChangeStream::kRegexAllCollections};
    const std::string firstCmdNs = "news.$cmd";
    const std::string secondCmdNs = "test.$cmd";

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"),
                         BSON(OR(BSON("ns" << BSON("$regex" << firstRegexNs)),
                                 BSON("ns" << BSON("$regex" << secondRegexNs)))))),
                BSON(AND(
                    fromjson("{op: {$eq: 'c'}}"),
                    BSON(OR(BSON(OR(BSON("o.renameCollection" << BSON("$regex" << firstRegexNs)),
                                    BSON("o.renameCollection" << BSON("$regex" << secondRegexNs)))),
                            BSON(OR(BSON("ns" << BSON("$eq" << firstCmdNs)),  // drop.
                                    BSON("ns" << BSON("$eq" << secondCmdNs)))),
                            BSON(OR(BSON("ns" << BSON("$eq" << firstCmdNs)),  // create.
                                    BSON("ns" << BSON("$eq" << secondCmdNs)))),
                            BSON(OR(BSON("ns" << BSON("$eq" << firstCmdNs)),  // createIndexes.
                                    BSON("ns" << BSON("$eq" << secondCmdNs)))),
                            BSON(OR(BSON("ns" << BSON("$eq" << firstCmdNs)),  // commitIndexBuild.
                                    BSON("ns" << BSON("$eq" << secondCmdNs)))),
                            BSON(OR(BSON("ns" << BSON("$eq" << firstCmdNs)),  // startIndexBuild.
                                    BSON("ns" << BSON("$eq" << secondCmdNs)))),
                            BSON(OR(BSON("ns" << BSON("$eq" << firstCmdNs)),  // abortIndexBuild.
                                    BSON("ns" << BSON("$eq" << secondCmdNs)))),
                            BSON(OR(BSON("ns" << BSON("$eq" << firstCmdNs)),  // dropIndexes.
                                    BSON("ns" << BSON("$eq" << secondCmdNs)))),
                            BSON(OR(BSON("ns" << BSON("$eq" << firstCmdNs)),  // collMod.
                                    BSON("ns" << BSON("$eq" << secondCmdNs)))),
                            BSON(AND(BSON(OR(BSON("ns" << BSON("$eq" << firstCmdNs)),
                                             BSON("ns" << BSON("$eq" << secondCmdNs)))),
                                     fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithNinExpressionOnDb) {
    auto expCtx = getExpCtx();
    auto expr = BSON("ns.db" << BSON("$nin" << BSON_ARRAY("test" << "news")));
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT(rewrittenMatchExpression);

    const std::string firstRegexNs = std::string("^") + "test" + "\\." +
        std::string{DocumentSourceChangeStream::kRegexAllCollections};
    const std::string secondRegexNs = std::string("^") + "news" + "\\." +
        std::string{DocumentSourceChangeStream::kRegexAllCollections};
    const std::string firstCmdNs = "test.$cmd";
    const std::string secondCmdNs = "news.$cmd";

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(NOR(BSON(OR(
            BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"),
                     BSON(OR(BSON("ns" << BSON("$regex" << secondRegexNs)),
                             BSON("ns" << BSON("$regex" << firstRegexNs)))))),
            BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                     BSON(OR(BSON(OR(BSON("o.renameCollection" << BSON("$regex" << secondRegexNs)),
                                     BSON("o.renameCollection" << BSON("$regex" << firstRegexNs)))),
                             BSON(OR(BSON("ns" << BSON("$eq" << secondCmdNs)),  // drop.
                                     BSON("ns" << BSON("$eq" << firstCmdNs)))),
                             BSON(OR(BSON("ns" << BSON("$eq" << secondCmdNs)),  // create.
                                     BSON("ns" << BSON("$eq" << firstCmdNs)))),
                             BSON(OR(BSON("ns" << BSON("$eq" << secondCmdNs)),  // createIndexes.
                                     BSON("ns" << BSON("$eq" << firstCmdNs)))),
                             BSON(OR(BSON("ns" << BSON("$eq" << secondCmdNs)),  // commitIndexBuild.
                                     BSON("ns" << BSON("$eq" << firstCmdNs)))),
                             BSON(OR(BSON("ns" << BSON("$eq" << secondCmdNs)),  // startIndexBuild.
                                     BSON("ns" << BSON("$eq" << firstCmdNs)))),
                             BSON(OR(BSON("ns" << BSON("$eq" << secondCmdNs)),  // abortIndexBuild.
                                     BSON("ns" << BSON("$eq" << firstCmdNs)))),
                             BSON(OR(BSON("ns" << BSON("$eq" << secondCmdNs)),  // dropIndexes.
                                     BSON("ns" << BSON("$eq" << firstCmdNs)))),
                             BSON(OR(BSON("ns" << BSON("$eq" << secondCmdNs)),  // collMod.
                                     BSON("ns" << BSON("$eq" << firstCmdNs)))),
                             BSON(AND(BSON(OR(BSON("ns" << BSON("$eq" << secondCmdNs)),
                                              BSON("ns" << BSON("$eq" << firstCmdNs)))),
                                      fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithInExpressionOnCollection) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'ns.coll': {$in: ['test', 'news']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT(rewrittenMatchExpression);

    const std::string firstRegexNs =
        std::string{DocumentSourceChangeStream::kRegexAllDBs} + "\\." + "news" + "$";
    const std::string secondRegexNs =
        std::string{DocumentSourceChangeStream::kRegexAllDBs} + "\\." + "test" + "$";

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"),
                         BSON(OR(BSON("ns" << BSON("$regex" << firstRegexNs)),
                                 BSON("ns" << BSON("$regex" << secondRegexNs)))))),
                BSON(AND(
                    fromjson("{op: {$eq: 'c'}}"),
                    BSON(OR(BSON(OR(BSON("o.renameCollection" << BSON("$regex" << firstRegexNs)),
                                    BSON("o.renameCollection" << BSON("$regex" << secondRegexNs)))),
                            BSON(OR(BSON("o.drop" << BSON("$eq" << "news")),
                                    BSON("o.drop" << BSON("$eq" << "test")))),
                            BSON(OR(BSON("o.create" << BSON("$eq" << "news")),
                                    BSON("o.create" << BSON("$eq" << "test")))),
                            BSON(OR(BSON("o.createIndexes" << BSON("$eq" << "news")),
                                    BSON("o.createIndexes" << BSON("$eq" << "test")))),
                            BSON(OR(BSON("o.commitIndexBuild" << BSON("$eq" << "news")),
                                    BSON("o.commitIndexBuild" << BSON("$eq" << "test")))),
                            BSON(OR(BSON("o.startIndexBuild" << BSON("$eq" << "news")),
                                    BSON("o.startIndexBuild" << BSON("$eq" << "test")))),
                            BSON(OR(BSON("o.abortIndexBuild" << BSON("$eq" << "news")),
                                    BSON("o.abortIndexBuild" << BSON("$eq" << "test")))),
                            BSON(OR(BSON("o.dropIndexes" << BSON("$eq" << "news")),
                                    BSON("o.dropIndexes" << BSON("$eq" << "test")))),
                            BSON(OR(BSON("o.collMod" << BSON("$eq" << "news")),
                                    BSON("o.collMod" << BSON("$eq" << "test")))),
                            BSON(AND(BSON(OR(fromjson("{$alwaysFalse: 1}"),
                                             fromjson("{$alwaysFalse: 1}"))),
                                     fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithNinExpressionOnCollection) {
    auto expCtx = getExpCtx();
    auto expr = BSON("ns.coll" << BSON("$nin" << BSON_ARRAY("test" << "news")));
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT(rewrittenMatchExpression);

    const std::string firstRegexNs =
        std::string{DocumentSourceChangeStream::kRegexAllDBs} + "\\." + "test" + "$";
    const std::string secondRegexNs =
        std::string{DocumentSourceChangeStream::kRegexAllDBs} + "\\." + "news" + "$";

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(NOR(BSON(OR(
            BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"),
                     BSON(OR(BSON("ns" << BSON("$regex" << secondRegexNs)),
                             BSON("ns" << BSON("$regex" << firstRegexNs)))))),
            BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                     BSON(OR(BSON(OR(BSON("o.renameCollection" << BSON("$regex" << secondRegexNs)),
                                     BSON("o.renameCollection" << BSON("$regex" << firstRegexNs)))),
                             BSON(OR(BSON("o.drop" << BSON("$eq" << "news")),
                                     BSON("o.drop" << BSON("$eq" << "test")))),
                             BSON(OR(BSON("o.create" << BSON("$eq" << "news")),
                                     BSON("o.create" << BSON("$eq" << "test")))),
                             BSON(OR(BSON("o.createIndexes" << BSON("$eq" << "news")),
                                     BSON("o.createIndexes" << BSON("$eq" << "test")))),
                             BSON(OR(BSON("o.commitIndexBuild" << BSON("$eq" << "news")),
                                     BSON("o.commitIndexBuild" << BSON("$eq" << "test")))),
                             BSON(OR(BSON("o.startIndexBuild" << BSON("$eq" << "news")),
                                     BSON("o.startIndexBuild" << BSON("$eq" << "test")))),
                             BSON(OR(BSON("o.abortIndexBuild" << BSON("$eq" << "news")),
                                     BSON("o.abortIndexBuild" << BSON("$eq" << "test")))),
                             BSON(OR(BSON("o.dropIndexes" << BSON("$eq" << "news")),
                                     BSON("o.dropIndexes" << BSON("$eq" << "test")))),
                             BSON(OR(BSON("o.collMod" << BSON("$eq" << "news")),
                                     BSON("o.collMod" << BSON("$eq" << "test")))),
                             BSON(AND(BSON(OR(fromjson("{$alwaysFalse: 1}"),
                                              fromjson("{$alwaysFalse: 1}"))),
                                      fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithInRegexExpressionOnDb) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'ns.db': {$in: [/^test.*$/, /^news$/]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(
            BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"),
                     BSON(OR(fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")),
                             fromjson(getNsDbRegexMatchExpr("$ns", R"(^news$)")))))),
            BSON(AND(
                fromjson("{op: {$eq: 'c'}}"),
                BSON(OR(
                    BSON(OR(fromjson(getNsDbRegexMatchExpr("$o.renameCollection", R"(^test.*$)")),
                            fromjson(getNsDbRegexMatchExpr("$o.renameCollection", R"(^news$)")))),
                    BSON(OR(fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")),  // drop.
                            fromjson(getNsDbRegexMatchExpr("$ns", R"(^news$)")))),
                    BSON(OR(fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")),  // create.
                            fromjson(getNsDbRegexMatchExpr("$ns", R"(^news$)")))),
                    BSON(
                        OR(fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")),  // createIndexes.
                           fromjson(getNsDbRegexMatchExpr("$ns", R"(^news$)")))),
                    BSON(OR(
                        fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")),  // commitIndexBuild.
                        fromjson(getNsDbRegexMatchExpr("$ns", R"(^news$)")))),
                    BSON(OR(
                        fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")),  // startIndexBuild.
                        fromjson(getNsDbRegexMatchExpr("$ns", R"(^news$)")))),
                    BSON(OR(
                        fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")),  // abortIndexBuild.
                        fromjson(getNsDbRegexMatchExpr("$ns", R"(^news$)")))),
                    BSON(OR(fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")),  // dropIndexes.
                            fromjson(getNsDbRegexMatchExpr("$ns", R"(^news$)")))),
                    BSON(OR(fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")),  // collMod.
                            fromjson(getNsDbRegexMatchExpr("$ns", R"(^news$)")))),
                    BSON(AND(BSON(OR(fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")),
                                     fromjson(getNsDbRegexMatchExpr("$ns", R"(^news$)")))),
                             fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithNinRegexExpressionOnDb) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'ns.db': {$nin: [/^test.*$/, /^news$/]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(NOR(BSON(OR(
            BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"),
                     BSON(OR(fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")),
                             fromjson(getNsDbRegexMatchExpr("$ns", R"(^news$)")))))),
            BSON(AND(
                fromjson("{op: {$eq: 'c'}}"),
                BSON(OR(
                    BSON(OR(fromjson(getNsDbRegexMatchExpr("$o.renameCollection", R"(^test.*$)")),
                            fromjson(getNsDbRegexMatchExpr("$o.renameCollection", R"(^news$)")))),
                    BSON(OR(fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")),  // drop.
                            fromjson(getNsDbRegexMatchExpr("$ns", R"(^news$)")))),
                    BSON(OR(fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")),  // create.
                            fromjson(getNsDbRegexMatchExpr("$ns", R"(^news$)")))),
                    BSON(
                        OR(fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")),  // createIndexes.
                           fromjson(getNsDbRegexMatchExpr("$ns", R"(^news$)")))),
                    BSON(OR(
                        fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")),  // commitIndexBuild.
                        fromjson(getNsDbRegexMatchExpr("$ns", R"(^news$)")))),
                    BSON(OR(
                        fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")),  // startIndexBuild.
                        fromjson(getNsDbRegexMatchExpr("$ns", R"(^news$)")))),
                    BSON(OR(
                        fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")),  // abortIndexBuild.
                        fromjson(getNsDbRegexMatchExpr("$ns", R"(^news$)")))),
                    BSON(OR(fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")),  // dropIndexes.
                            fromjson(getNsDbRegexMatchExpr("$ns", R"(^news$)")))),
                    BSON(OR(fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")),  // collMod.
                            fromjson(getNsDbRegexMatchExpr("$ns", R"(^news$)")))),
                    BSON(AND(BSON(OR(fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")),
                                     fromjson(getNsDbRegexMatchExpr("$ns", R"(^news$)")))),
                             fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithInRegexExpressionOnCollection) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'ns.coll': {$in: [/^test.*$/, /^news$/]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(
            BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"),
                     BSON(OR(fromjson(getNsCollRegexMatchExpr("$ns", R"(^test.*$)")),
                             fromjson(getNsCollRegexMatchExpr("$ns", R"(^news$)")))))),
            BSON(AND(
                fromjson("{op: {$eq: 'c'}}"),
                BSON(OR(
                    BSON(OR(fromjson(getNsCollRegexMatchExpr("$o.renameCollection", R"(^test.*$)")),
                            fromjson(getNsCollRegexMatchExpr("$o.renameCollection", R"(^news$)")))),
                    BSON(OR(fromjson(getNsCollRegexMatchExpr("$o.drop", R"(^test.*$)")),
                            fromjson(getNsCollRegexMatchExpr("$o.drop", R"(^news$)")))),
                    BSON(OR(fromjson(getNsCollRegexMatchExpr("$o.create", R"(^test.*$)")),
                            fromjson(getNsCollRegexMatchExpr("$o.create", R"(^news$)")))),
                    BSON(OR(fromjson(getNsCollRegexMatchExpr("$o.createIndexes", R"(^test.*$)")),
                            fromjson(getNsCollRegexMatchExpr("$o.createIndexes", R"(^news$)")))),
                    BSON(OR(fromjson(getNsCollRegexMatchExpr("$o.commitIndexBuild", R"(^test.*$)")),
                            fromjson(getNsCollRegexMatchExpr("$o.commitIndexBuild", R"(^news$)")))),
                    BSON(OR(fromjson(getNsCollRegexMatchExpr("$o.startIndexBuild", R"(^test.*$)")),
                            fromjson(getNsCollRegexMatchExpr("$o.startIndexBuild", R"(^news$)")))),
                    BSON(OR(fromjson(getNsCollRegexMatchExpr("$o.abortIndexBuild", R"(^test.*$)")),
                            fromjson(getNsCollRegexMatchExpr("$o.abortIndexBuild", R"(^news$)")))),
                    BSON(OR(fromjson(getNsCollRegexMatchExpr("$o.dropIndexes", R"(^test.*$)")),
                            fromjson(getNsCollRegexMatchExpr("$o.dropIndexes", R"(^news$)")))),
                    BSON(OR(fromjson(getNsCollRegexMatchExpr("$o.collMod", R"(^test.*$)")),
                            fromjson(getNsCollRegexMatchExpr("$o.collMod", R"(^news$)")))),
                    BSON(AND(BSON(OR(fromjson("{$alwaysFalse: 1}"), fromjson("{$alwaysFalse: 1}"))),
                             fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithNinRegexExpressionOnCollection) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'ns.coll': {$nin: [/^test.*$/, /^news$/]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(NOR(BSON(OR(
            BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"),
                     BSON(OR(fromjson(getNsCollRegexMatchExpr("$ns", R"(^test.*$)")),
                             fromjson(getNsCollRegexMatchExpr("$ns", R"(^news$)")))))),
            BSON(AND(
                fromjson("{op: {$eq: 'c'}}"),
                BSON(OR(
                    BSON(OR(fromjson(getNsCollRegexMatchExpr("$o.renameCollection", R"(^test.*$)")),
                            fromjson(getNsCollRegexMatchExpr("$o.renameCollection", R"(^news$)")))),
                    BSON(OR(fromjson(getNsCollRegexMatchExpr("$o.drop", R"(^test.*$)")),
                            fromjson(getNsCollRegexMatchExpr("$o.drop", R"(^news$)")))),
                    BSON(OR(fromjson(getNsCollRegexMatchExpr("$o.create", R"(^test.*$)")),
                            fromjson(getNsCollRegexMatchExpr("$o.create", R"(^news$)")))),
                    BSON(OR(fromjson(getNsCollRegexMatchExpr("$o.createIndexes", R"(^test.*$)")),
                            fromjson(getNsCollRegexMatchExpr("$o.createIndexes", R"(^news$)")))),
                    BSON(OR(fromjson(getNsCollRegexMatchExpr("$o.commitIndexBuild", R"(^test.*$)")),
                            fromjson(getNsCollRegexMatchExpr("$o.commitIndexBuild", R"(^news$)")))),
                    BSON(OR(fromjson(getNsCollRegexMatchExpr("$o.startIndexBuild", R"(^test.*$)")),
                            fromjson(getNsCollRegexMatchExpr("$o.startIndexBuild", R"(^news$)")))),
                    BSON(OR(fromjson(getNsCollRegexMatchExpr("$o.abortIndexBuild", R"(^test.*$)")),
                            fromjson(getNsCollRegexMatchExpr("$o.abortIndexBuild", R"(^news$)")))),
                    BSON(OR(fromjson(getNsCollRegexMatchExpr("$o.dropIndexes", R"(^test.*$)")),
                            fromjson(getNsCollRegexMatchExpr("$o.dropIndexes", R"(^news$)")))),
                    BSON(OR(fromjson(getNsCollRegexMatchExpr("$o.collMod", R"(^test.*$)")),
                            fromjson(getNsCollRegexMatchExpr("$o.collMod", R"(^news$)")))),
                    BSON(AND(BSON(OR(fromjson("{$alwaysFalse: 1}"), fromjson("{$alwaysFalse: 1}"))),
                             fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithInExpressionOnDbWithRegexAndString) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'ns.db': {$in: [/^test.*$/, 'news']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT(rewrittenMatchExpression);

    const std::string secondRegexNs = std::string("^") + "news" + "\\." +
        std::string{DocumentSourceChangeStream::kRegexAllCollections};
    const std::string secondCmdNs = "news.$cmd";

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(
            BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"),
                     BSON(OR(BSON("ns" << BSON("$regex" << secondRegexNs)),
                             fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")))))),
            BSON(AND(
                fromjson("{op: {$eq: 'c'}}"),
                BSON(OR(
                    BSON(OR(BSON("o.renameCollection" << BSON("$regex" << secondRegexNs)),
                            fromjson(getNsDbRegexMatchExpr("$o.renameCollection", R"(^test.*$)")))),
                    BSON(OR(BSON("ns" << BSON("$eq" << secondCmdNs)),
                            fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")))),  // drop.
                    BSON(OR(BSON("ns" << BSON("$eq" << secondCmdNs)),
                            fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")))),  // create.
                    BSON(OR(
                        BSON("ns" << BSON("$eq" << secondCmdNs)),
                        fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")))),  // createIndexes.
                    BSON(OR(BSON("ns" << BSON("$eq" << secondCmdNs)),
                            fromjson(getNsDbRegexMatchExpr("$ns",
                                                           R"(^test.*$)")))),  // commitIndexBuild.
                    BSON(OR(BSON("ns" << BSON("$eq" << secondCmdNs)),
                            fromjson(getNsDbRegexMatchExpr("$ns",
                                                           R"(^test.*$)")))),  // startIndexBuild.
                    BSON(OR(BSON("ns" << BSON("$eq" << secondCmdNs)),
                            fromjson(getNsDbRegexMatchExpr("$ns",
                                                           R"(^test.*$)")))),  // abortIndexBuild.
                    BSON(
                        OR(BSON("ns" << BSON("$eq" << secondCmdNs)),
                           fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")))),  // dropIndexes.
                    BSON(OR(BSON("ns" << BSON("$eq" << secondCmdNs)),
                            fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")))),  // collMod.
                    BSON(AND(BSON(OR(BSON("ns" << BSON("$eq" << secondCmdNs)),
                                     fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")))),
                             fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithNinExpressionOnDbWithRegexAndString) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'ns.db': {$nin: [/^test.*$/, 'news']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT(rewrittenMatchExpression);


    const std::string secondRegexNs = std::string("^") + "news" + "\\." +
        std::string{DocumentSourceChangeStream::kRegexAllCollections};
    const std::string secondCmdNs = "news.$cmd";

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(NOR(BSON(OR(
            BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"),
                     BSON(OR(BSON("ns" << BSON("$regex" << secondRegexNs)),
                             fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")))))),
            BSON(AND(
                fromjson("{op: {$eq: 'c'}}"),
                BSON(OR(
                    BSON(OR(BSON("o.renameCollection" << BSON("$regex" << secondRegexNs)),
                            fromjson(getNsDbRegexMatchExpr("$o.renameCollection", R"(^test.*$)")))),
                    BSON(OR(BSON("ns" << BSON("$eq" << secondCmdNs)),
                            fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")))),  // drop.
                    BSON(OR(BSON("ns" << BSON("$eq" << secondCmdNs)),
                            fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")))),  // create.
                    BSON(OR(
                        BSON("ns" << BSON("$eq" << secondCmdNs)),
                        fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")))),  // createIndexes.
                    BSON(OR(BSON("ns" << BSON("$eq" << secondCmdNs)),
                            fromjson(getNsDbRegexMatchExpr("$ns",
                                                           R"(^test.*$)")))),  // commitIndexBuild.
                    BSON(OR(BSON("ns" << BSON("$eq" << secondCmdNs)),
                            fromjson(getNsDbRegexMatchExpr("$ns",
                                                           R"(^test.*$)")))),  // startIndexBuild.
                    BSON(OR(BSON("ns" << BSON("$eq" << secondCmdNs)),
                            fromjson(getNsDbRegexMatchExpr("$ns",
                                                           R"(^test.*$)")))),  // abortIndexBuild.
                    BSON(
                        OR(BSON("ns" << BSON("$eq" << secondCmdNs)),
                           fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")))),  // dropIndexes.
                    BSON(OR(BSON("ns" << BSON("$eq" << secondCmdNs)),
                            fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")))),  // collMod.
                    BSON(AND(BSON(OR(BSON("ns" << BSON("$eq" << secondCmdNs)),
                                     fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")))),
                             fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithInExpressionOnCollectionWithRegexAndString) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'ns.coll': {$in: [/^test.*$/, 'news']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT(rewrittenMatchExpression);

    const std::string secondRegexNs =
        DocumentSourceChangeStream::kRegexAllDBs + "\\." + "news" + "$";
    const std::string secondCmdNs = "news.$cmd";

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(
            BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"),
                     BSON(OR(BSON("ns" << BSON("$regex" << secondRegexNs)),
                             fromjson(getNsCollRegexMatchExpr("$ns", R"(^test.*$)")))))),
            BSON(AND(
                fromjson("{op: {$eq: 'c'}}"),
                BSON(OR(
                    BSON(OR(
                        BSON("o.renameCollection" << BSON("$regex" << secondRegexNs)),
                        fromjson(getNsCollRegexMatchExpr("$o.renameCollection", R"(^test.*$)")))),
                    BSON(OR(BSON("o.drop" << BSON("$eq" << "news")),
                            fromjson(getNsCollRegexMatchExpr("$o.drop", R"(^test.*$)")))),
                    BSON(OR(BSON("o.create" << BSON("$eq" << "news")),
                            fromjson(getNsCollRegexMatchExpr("$o.create", R"(^test.*$)")))),
                    BSON(OR(BSON("o.createIndexes" << BSON("$eq" << "news")),
                            fromjson(getNsCollRegexMatchExpr("$o.createIndexes", R"(^test.*$)")))),
                    BSON(OR(
                        BSON("o.commitIndexBuild" << BSON("$eq" << "news")),
                        fromjson(getNsCollRegexMatchExpr("$o.commitIndexBuild", R"(^test.*$)")))),
                    BSON(
                        OR(BSON("o.startIndexBuild" << BSON("$eq" << "news")),
                           fromjson(getNsCollRegexMatchExpr("$o.startIndexBuild", R"(^test.*$)")))),
                    BSON(
                        OR(BSON("o.abortIndexBuild" << BSON("$eq" << "news")),
                           fromjson(getNsCollRegexMatchExpr("$o.abortIndexBuild", R"(^test.*$)")))),
                    BSON(OR(BSON("o.dropIndexes" << BSON("$eq" << "news")),
                            fromjson(getNsCollRegexMatchExpr("$o.dropIndexes", R"(^test.*$)")))),
                    BSON(OR(BSON("o.collMod" << BSON("$eq" << "news")),
                            fromjson(getNsCollRegexMatchExpr("$o.collMod", R"(^test.*$)")))),
                    BSON(AND(BSON(OR(fromjson("{$alwaysFalse: 1}"), fromjson("{$alwaysFalse: 1}"))),
                             fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest,
       CanRewriteNamespaceWithNinExpressionOnCollectionWithRegexAndString) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'ns.coll': {$nin: [/^test.*$/, 'news']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT(rewrittenMatchExpression);

    const std::string secondRegexNs =
        DocumentSourceChangeStream::kRegexAllDBs + "\\." + "news" + "$";
    const std::string secondCmdNs = "news.$cmd";

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(NOR(BSON(OR(
            BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"),
                     BSON(OR(BSON("ns" << BSON("$regex" << secondRegexNs)),
                             fromjson(getNsCollRegexMatchExpr("$ns", R"(^test.*$)")))))),
            BSON(AND(
                fromjson("{op: {$eq: 'c'}}"),
                BSON(OR(
                    BSON(OR(
                        BSON("o.renameCollection" << BSON("$regex" << secondRegexNs)),
                        fromjson(getNsCollRegexMatchExpr("$o.renameCollection", R"(^test.*$)")))),
                    BSON(OR(BSON("o.drop" << BSON("$eq" << "news")),
                            fromjson(getNsCollRegexMatchExpr("$o.drop", R"(^test.*$)")))),
                    BSON(OR(BSON("o.create" << BSON("$eq" << "news")),
                            fromjson(getNsCollRegexMatchExpr("$o.create", R"(^test.*$)")))),
                    BSON(OR(BSON("o.createIndexes" << BSON("$eq" << "news")),
                            fromjson(getNsCollRegexMatchExpr("$o.createIndexes", R"(^test.*$)")))),
                    BSON(OR(
                        BSON("o.commitIndexBuild" << BSON("$eq" << "news")),
                        fromjson(getNsCollRegexMatchExpr("$o.commitIndexBuild", R"(^test.*$)")))),
                    BSON(
                        OR(BSON("o.startIndexBuild" << BSON("$eq" << "news")),
                           fromjson(getNsCollRegexMatchExpr("$o.startIndexBuild", R"(^test.*$)")))),
                    BSON(
                        OR(BSON("o.abortIndexBuild" << BSON("$eq" << "news")),
                           fromjson(getNsCollRegexMatchExpr("$o.abortIndexBuild", R"(^test.*$)")))),
                    BSON(OR(BSON("o.dropIndexes" << BSON("$eq" << "news")),
                            fromjson(getNsCollRegexMatchExpr("$o.dropIndexes", R"(^test.*$)")))),
                    BSON(OR(BSON("o.collMod" << BSON("$eq" << "news")),
                            fromjson(getNsCollRegexMatchExpr("$o.collMod", R"(^test.*$)")))),
                    BSON(AND(BSON(OR(fromjson("{$alwaysFalse: 1}"), fromjson("{$alwaysFalse: 1}"))),
                             fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))))));
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteNamespaceWithInExpressionOnInvalidDb) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'ns.db': {$in: ['test', 1]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT_FALSE(rewrittenMatchExpression);
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteNamespaceWithNinExpressionOnInvalidDb) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'ns.db': {$nin: ['test', 1]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT_FALSE(rewrittenMatchExpression);
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteNamespaceWithInExpressionOnInvalidCollection) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'ns.coll': {$in: ['coll', 1]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT_FALSE(rewrittenMatchExpression);
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteNamespaceWithNinExpressionOnInvalidCollection) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'ns.coll': {$nin: ['coll', 1]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT_FALSE(rewrittenMatchExpression);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithEmptyInExpression) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'ns.db': {$in: []}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"), fromjson("{$alwaysFalse: 1 }"))),
                BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                         BSON(OR(fromjson("{$alwaysFalse: 1 }"),  // rename.
                                 fromjson("{$alwaysFalse: 1 }"),  // drop.
                                 fromjson("{$alwaysFalse: 1 }"),  // create.
                                 fromjson("{$alwaysFalse: 1 }"),  // createIndexes.
                                 fromjson("{$alwaysFalse: 1 }"),  // commitIndexBuild.
                                 fromjson("{$alwaysFalse: 1 }"),  // startIndexBuild.
                                 fromjson("{$alwaysFalse: 1 }"),  // abortIndexBuild.
                                 fromjson("{$alwaysFalse: 1 }"),  // dropIndexes.
                                 fromjson("{$alwaysFalse: 1 }"),  // collMod.
                                 BSON(AND(fromjson("{$alwaysFalse: 1 }"),
                                          fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithEmptyNinExpression) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'ns.db': {$nin: []}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(NOR(
            BSON(OR(BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"), fromjson("{$alwaysFalse: 1 }"))),
                    BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                             BSON(OR(fromjson("{$alwaysFalse: 1 }"),  // rename.
                                     fromjson("{$alwaysFalse: 1 }"),  // drop.
                                     fromjson("{$alwaysFalse: 1 }"),  // create.
                                     fromjson("{$alwaysFalse: 1 }"),  // createIndexes.
                                     fromjson("{$alwaysFalse: 1 }"),  // commitIndexBuild.
                                     fromjson("{$alwaysFalse: 1 }"),  // startIndexBuild.
                                     fromjson("{$alwaysFalse: 1 }"),  // abortIndexBuild.
                                     fromjson("{$alwaysFalse: 1 }"),  // dropIndexes.
                                     fromjson("{$alwaysFalse: 1 }"),  // collMod.
                                     BSON(AND(fromjson("{$alwaysFalse: 1 }"),
                                              fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNsWithExprOnFullObject) {
    auto expCtx = getExpCtx();
    auto expr =
        fromjson("{$expr: {$eq: ['$ns', {db: '" + expCtx->getNamespaceString().db_forTest() +
                 "', coll: '" + expCtx->getNamespaceString().coll() + "'}]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto caseCRUD =
        "{case: {$in: ['$op', [{$const: 'i' }, {$const: 'u' }, {$const: 'd'}]]}, then: "
        "{$substrBytes: ['$ns', {$add: [{$strLenBytes: ['$$dbName']}, {$const: 1}]}, {$const: "
        "-1}]}}"s;
    auto caseNotCmd = "{case: { $ne: ['$op', {$const: 'c'}]}, then: '$$REMOVE'}";
    auto caseDropDatabase = "{case: {$ne: ['$o.dropDatabase', '$$REMOVE']}, then: '$$REMOVE'}";
    auto caseRenameCollection =
        "{case: {$ne: ['$o.renameCollection', '$$REMOVE']}, then: {$substrBytes: "
        "['$o.renameCollection', {$add: [{$strLenBytes: ['$$dbName']}, {$const: 1}]}, {$const: "
        "-1}]}}";
    auto caseDrop = "{case: {$ne: ['$o.drop', '$$REMOVE']}, then: '$o.drop'}";
    auto caseCreate = "{case: {$ne: ['$o.create', '$$REMOVE']}, then: '$o.create'}";
    auto caseCreateIndexes =
        "{case: {$ne: ['$o.createIndexes', '$$REMOVE']}, then: '$o.createIndexes'}";
    auto caseCommitIndexBuild =
        "{case: {$ne: ['$o.commitIndexBuild', '$$REMOVE']}, then: '$o.commitIndexBuild'}";
    auto caseStartIndexBuild =
        "{case: {$ne: ['$o.startIndexBuild', '$$REMOVE']}, then: '$o.startIndexBuild'}";
    auto caseAbortIndexBuild =
        "{case: {$ne: ['$o.abortIndexBuild', '$$REMOVE']}, then: '$o.abortIndexBuild'}";
    auto caseDropIndexes = "{case: {$ne: ['$o.dropIndexes', '$$REMOVE']}, then: '$o.dropIndexes'}";
    auto caseCollMod = "{case: {$ne: ['$o.collMod', '$$REMOVE']}, then: '$o.collMod'}";

    const auto cases = boost::algorithm::join(std::vector<std::string>{caseCRUD,
                                                                       caseNotCmd,
                                                                       caseDropDatabase,
                                                                       caseRenameCollection,
                                                                       caseDrop,
                                                                       caseCreate,
                                                                       caseCreateIndexes,
                                                                       caseCommitIndexBuild,
                                                                       caseStartIndexBuild,
                                                                       caseAbortIndexBuild,
                                                                       caseDropIndexes,
                                                                       caseCollMod},
                                              ",");

    auto expectedExpr = fromjson(
        "{"
        "  $expr: {"
        "    $eq: [{"
        "      $let: {"
        "        vars: {"
        "          dbName: {$substrBytes: ["
        "            '$ns', "
        "            {$const: 0}, "
        "            {$indexOfBytes: ['$ns', {$const: '.'}]}]}}, "
        "        in: {"
        "          db: '$$dbName',"
        "          coll: {"
        "            $switch: {"
        "              branches: ["s +
        cases +
        "              ], default: '$$REMOVE'}}}}}, "
        "      {db: {$const: 'unittests' }, coll: {$const: 'pipeline_test'}}]}"
        "}");

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, expectedExpr);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNsWithExprOnFullObjectWithOnlyDb) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{$expr: {$eq: ['$ns', {db: '" +
                         expCtx->getNamespaceString().db_forTest() + "'}]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto caseCRUD =
        "{case: {$in: ['$op', [{$const: 'i' }, {$const: 'u' }, {$const: 'd'}]]}, then: "
        "{$substrBytes: ['$ns', {$add: [{$strLenBytes: ['$$dbName']}, {$const: 1}]}, {$const: "
        "-1}]}}"s;
    auto caseNotCmd = "{case: { $ne: ['$op', {$const: 'c'}]}, then: '$$REMOVE'}";
    auto caseDropDatabase = "{case: {$ne: ['$o.dropDatabase', '$$REMOVE']}, then: '$$REMOVE'}";
    auto caseRenameCollection =
        "{case: {$ne: ['$o.renameCollection', '$$REMOVE']}, then: {$substrBytes: "
        "['$o.renameCollection', {$add: [{$strLenBytes: ['$$dbName']}, {$const: 1}]}, {$const: "
        "-1}]}}";
    auto caseDrop = "{case: {$ne: ['$o.drop', '$$REMOVE']}, then: '$o.drop'}";
    auto caseCreate = "{case: {$ne: ['$o.create', '$$REMOVE']}, then: '$o.create'}";
    auto caseCreateIndexes =
        "{case: {$ne: ['$o.createIndexes', '$$REMOVE']}, then: '$o.createIndexes'}";
    auto caseCommitIndexBuild =
        "{case: {$ne: ['$o.commitIndexBuild', '$$REMOVE']}, then: '$o.commitIndexBuild'}";
    auto caseStartIndexBuild =
        "{case: {$ne: ['$o.startIndexBuild', '$$REMOVE']}, then: '$o.startIndexBuild'}";
    auto caseAbortIndexBuild =
        "{case: {$ne: ['$o.abortIndexBuild', '$$REMOVE']}, then: '$o.abortIndexBuild'}";
    auto caseDropIndexes = "{case: {$ne: ['$o.dropIndexes', '$$REMOVE']}, then: '$o.dropIndexes'}";
    auto caseCollMod = "{case: {$ne: ['$o.collMod', '$$REMOVE']}, then: '$o.collMod'}";

    const auto cases = boost::algorithm::join(std::vector<std::string>{caseCRUD,
                                                                       caseNotCmd,
                                                                       caseDropDatabase,
                                                                       caseRenameCollection,
                                                                       caseDrop,
                                                                       caseCreate,
                                                                       caseCreateIndexes,
                                                                       caseCommitIndexBuild,
                                                                       caseStartIndexBuild,
                                                                       caseAbortIndexBuild,
                                                                       caseDropIndexes,
                                                                       caseCollMod},
                                              ",");

    auto expectedExpr = fromjson(
        "{"
        "  $expr: {"
        "    $eq: [{"
        "      $let: {"
        "        vars: {"
        "          dbName: {$substrBytes: ["
        "            '$ns', "
        "            {$const: 0}, "
        "            {$indexOfBytes: ['$ns', {$const: '.'}]}]}}, "
        "        in: {"
        "          db: '$$dbName',"
        "          coll: {"
        "            $switch: {"
        "              branches: ["s +
        cases +
        "              ], default: '$$REMOVE'}}}}}, "
        "      {db: {$const: 'unittests' }}]}"
        "}");

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, expectedExpr);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNsWithExprOnDbFieldPath) {
    auto expCtx = getExpCtx();
    auto expr =
        fromjson("{$expr: {$eq: ['$ns.db', '" + expCtx->getNamespaceString().coll() + "']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto expectedExpr = fromjson(
        "{"
        "  $expr: {"
        "    $eq: [{"
        "      $let: {"
        "        vars: {"
        "          dbName: {$substrBytes: ["
        "            '$ns', "
        "            {$const: 0}, "
        "            {$indexOfBytes: ['$ns', {$const: '.'}]}]}}, "
        "        in: '$$dbName' }},"
        "      {$const: 'pipeline_test'}]}"
        "}");

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, expectedExpr);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNsWithExprOnCollFieldPath) {
    auto expCtx = getExpCtx();
    auto expr =
        fromjson("{$expr: {$eq: ['$ns.coll', '" + expCtx->getNamespaceString().coll() + "']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto caseCRUD =
        "{case: {$in: ['$op', [{$const: 'i' }, {$const: 'u' }, {$const: 'd'}]]}, then: "
        "{$substrBytes: ['$ns', {$add: [{$strLenBytes: ['$$dbName']}, {$const: 1}]}, {$const: "
        "-1}]}}"s;
    auto caseNotCmd = "{case: { $ne: ['$op', {$const: 'c'}]}, then: '$$REMOVE'}";
    auto caseDropDatabase = "{case: {$ne: ['$o.dropDatabase', '$$REMOVE']}, then: '$$REMOVE'}";
    auto caseRenameCollection =
        "{case: {$ne: ['$o.renameCollection', '$$REMOVE']}, then: {$substrBytes: "
        "['$o.renameCollection', {$add: [{$strLenBytes: ['$$dbName']}, {$const: 1}]}, {$const: "
        "-1}]}}";
    auto caseDrop = "{case: {$ne: ['$o.drop', '$$REMOVE']}, then: '$o.drop'}";
    auto caseCreate = "{case: {$ne: ['$o.create', '$$REMOVE']}, then: '$o.create'}";
    auto caseCreateIndexes =
        "{case: {$ne: ['$o.createIndexes', '$$REMOVE']}, then: '$o.createIndexes'}";
    auto caseCommitIndexBuild =
        "{case: {$ne: ['$o.commitIndexBuild', '$$REMOVE']}, then: '$o.commitIndexBuild'}";
    auto caseStartIndexBuild =
        "{case: {$ne: ['$o.startIndexBuild', '$$REMOVE']}, then: '$o.startIndexBuild'}";
    auto caseAbortIndexBuild =
        "{case: {$ne: ['$o.abortIndexBuild', '$$REMOVE']}, then: '$o.abortIndexBuild'}";
    auto caseDropIndexes = "{case: {$ne: ['$o.dropIndexes', '$$REMOVE']}, then: '$o.dropIndexes'}";
    auto caseCollMod = "{case: {$ne: ['$o.collMod', '$$REMOVE']}, then: '$o.collMod'}";

    const auto cases = boost::algorithm::join(std::vector<std::string>{caseCRUD,
                                                                       caseNotCmd,
                                                                       caseDropDatabase,
                                                                       caseRenameCollection,
                                                                       caseDrop,
                                                                       caseCreate,
                                                                       caseCreateIndexes,
                                                                       caseCommitIndexBuild,
                                                                       caseStartIndexBuild,
                                                                       caseAbortIndexBuild,
                                                                       caseDropIndexes,
                                                                       caseCollMod},
                                              ",");

    auto expectedExpr = fromjson(
        "{"
        "  $expr: {"
        "    $eq: [{"
        "      $let: {"
        "        vars: {"
        "          dbName: {$substrBytes: ["
        "            '$ns', "
        "            {$const: 0}, "
        "            {$indexOfBytes: ['$ns', {$const: '.'}]}]}}, "
        "        in: {"
        "            $switch: {"
        "              branches: ["s +
        cases +
        "              ], default: '$$REMOVE'}}}}, "
        "       {$const: 'pipeline_test'}]}"
        "}");

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, expectedExpr);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNsWithExprOnInvalidFieldPath) {
    auto expCtx = getExpCtx();
    auto expr =
        fromjson("{$expr: {$eq: ['$ns.test', '" + expCtx->getNamespaceString().coll() + "']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$expr: {$eq: ['$$REMOVE', {$const: 'pipeline_test'}]}}"));
}

//
// 'to' rewrites
//
TEST_F(ChangeStreamRewriteTest, CanRewriteFullToObject) {
    auto expCtx = getExpCtx();
    auto query = BSON("to" << BSON("db" << expCtx->getNamespaceString().db_forTest() << "coll"
                                        << expCtx->getNamespaceString().coll()));
    auto statusWithMatchExpression = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    const std::string ns = std::string{expCtx->getNamespaceString().db_forTest()} + "." +
        std::string{expCtx->getNamespaceString().coll()};

    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(AND(fromjson("{op: {$eq: 'c'}}"), BSON("o.to" << BSON("$eq" << ns)))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToObjectWithSwappedField) {
    auto expCtx = getExpCtx();
    auto query = BSON("to" << BSON("coll" << expCtx->getNamespaceString().coll() << "db"
                                          << expCtx->getNamespaceString().db_forTest()));
    auto statusWithMatchExpression = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(AND(fromjson("{op: {$eq: 'c'}}"), fromjson("{$alwaysFalse: 1 }"))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToObjectWithOnlyDbField) {
    auto expCtx = getExpCtx();
    auto query = BSON("to" << BSON("db" << expCtx->getNamespaceString().db_forTest()));
    auto statusWithMatchExpression = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(AND(fromjson("{op: {$eq: 'c'}}"), fromjson("{$alwaysFalse: 1 }"))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToObjectWithOnlyCollectionField) {
    auto expCtx = getExpCtx();
    auto query = BSON("to" << BSON("coll" << expCtx->getNamespaceString().coll()));
    auto statusWithMatchExpression = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(AND(fromjson("{op: {$eq: 'c'}}"), fromjson("{$alwaysFalse: 1 }"))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToObjectWithInvalidDbField) {
    auto expCtx = getExpCtx();
    auto query = BSON("to" << BSON("db" << 1 << "coll" << expCtx->getNamespaceString().coll()));
    auto statusWithMatchExpression = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(AND(fromjson("{op: {$eq: 'c'}}"), fromjson("{$alwaysFalse: 1 }"))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToObjectWithInvalidCollField) {
    auto expCtx = getExpCtx();
    auto query =
        BSON("to" << BSON("db" << expCtx->getNamespaceString().db_forTest() << "coll" << 1));
    auto statusWithMatchExpression = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(AND(fromjson("{op: {$eq: 'c'}}"), fromjson("{$alwaysFalse: 1 }"))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToObjectWithExtraField) {
    auto expCtx = getExpCtx();
    auto query = fromjson("{to: {db: 'db', coll: 'coll', extra: 'extra'}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(AND(fromjson("{op: {$eq: 'c'}}"), fromjson("{$alwaysFalse: 1 }"))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithStringDbFieldPath) {
    auto expCtx = getExpCtx();
    auto query = BSON("to.db" << expCtx->getNamespaceString().db_forTest());
    auto statusWithMatchExpression = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    const std::string regexNs = "^" + std::string{expCtx->getNamespaceString().db_forTest()} +
        "\\." + std::string{DocumentSourceChangeStream::kRegexAllCollections};

    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(AND(fromjson("{op: {$eq: 'c'}}"), BSON("o.to" << BSON("$regex" << regexNs)))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithCollectionFieldPath) {
    auto expCtx = getExpCtx();
    auto query = BSON("to.coll" << expCtx->getNamespaceString().coll());
    auto statusWithMatchExpression = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    const std::string regexNs = std::string{DocumentSourceChangeStream::kRegexAllDBs} + "\\." +
        std::string{expCtx->getNamespaceString().coll()} + "$";

    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(AND(fromjson("{op: {$eq: 'c'}}"), BSON("o.to" << BSON("$regex" << regexNs)))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithRegexDbFieldPath) {
    auto expCtx = getExpCtx();
    auto query = BSON("to.db" << BSONRegEx(R"(^unit.*$)"));
    auto statusWithMatchExpression = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                               fromjson(getNsDbRegexMatchExpr("$o.to", R"(^unit.*$)")))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithRegexCollectionFieldPath) {
    auto expCtx = getExpCtx();
    auto query = BSON("to.coll" << BSONRegEx(R"(^pipeline.*$)"));
    auto statusWithMatchExpression = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                               fromjson(getNsCollRegexMatchExpr("$o.to", R"(^pipeline.*$)")))));
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteToWithInvalidDbFieldPath) {
    auto expCtx = getExpCtx();
    auto query = BSON("to.db" << 1);
    auto statusWithMatchExpression = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT_FALSE(rewrittenMatchExpression);
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteToWithInvalidCollectionFieldPath) {
    auto expCtx = getExpCtx();
    auto query = BSON("to.coll" << 1);
    auto statusWithMatchExpression = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT_FALSE(rewrittenMatchExpression);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithExtraDbFieldPath) {
    auto expCtx = getExpCtx();
    auto query = BSON("to.to.subField" << "subDb");
    auto statusWithMatchExpression = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(AND(fromjson("{op: {$eq: 'c'}}"), fromjson("{$alwaysFalse: 1 }"))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithExtraCollectionFieldPath) {
    auto expCtx = getExpCtx();
    auto query = BSON("to.coll.subField" << "subColl");
    auto statusWithMatchExpression = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(AND(fromjson("{op: {$eq: 'c'}}"), fromjson("{$alwaysFalse: 1 }"))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithInvalidFieldPath) {
    auto expCtx = getExpCtx();
    auto query = BSON("to.unknown" << "test");
    auto statusWithMatchExpression = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(AND(fromjson("{op: {$eq: 'c'}}"), fromjson("{$alwaysFalse: 1 }"))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithInExpressionOnDb) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'to.db': {$in: ['test', 'news']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT(rewrittenMatchExpression);

    const std::string firstRegexNs = std::string("^") + "news" + "\\." +
        std::string{DocumentSourceChangeStream::kRegexAllCollections};
    const std::string secondRegexNs = std::string("^") + "test" + "\\." +
        std::string{DocumentSourceChangeStream::kRegexAllCollections};

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                               BSON(OR(BSON("o.to" << BSON("$regex" << firstRegexNs)),
                                       BSON("o.to" << BSON("$regex" << secondRegexNs)))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithNinExpressionOnDb) {
    auto expCtx = getExpCtx();
    auto expr = BSON("to.db" << BSON("$nin" << BSON_ARRAY("test" << "news")));
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT(rewrittenMatchExpression);

    const std::string firstRegexNs = std::string("^") + "test" + "\\." +
        std::string{DocumentSourceChangeStream::kRegexAllCollections};
    const std::string secondRegexNs = std::string("^") + "news" + "\\." +
        std::string{DocumentSourceChangeStream::kRegexAllCollections};

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(NOR(BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                          BSON(OR(BSON("o.to" << BSON("$regex" << secondRegexNs)),
                                  BSON("o.to" << BSON("$regex" << firstRegexNs)))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithInExpressionOnCollection) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'to.coll': {$in: ['test', 'news']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT(rewrittenMatchExpression);

    const std::string firstRegexNs =
        std::string{DocumentSourceChangeStream::kRegexAllDBs} + "\\." + "news" + "$";
    const std::string secondRegexNs =
        std::string{DocumentSourceChangeStream::kRegexAllDBs} + "\\." + "test" + "$";

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                               BSON(OR(BSON("o.to" << BSON("$regex" << firstRegexNs)),
                                       BSON("o.to" << BSON("$regex" << secondRegexNs)))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithNinExpressionOnCollection) {
    auto expCtx = getExpCtx();
    auto expr = BSON("to.coll" << BSON("$nin" << BSON_ARRAY("test" << "news")));
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT(rewrittenMatchExpression);

    const std::string firstRegexNs =
        std::string{DocumentSourceChangeStream::kRegexAllDBs} + "\\." + "test" + "$";
    const std::string secondRegexNs =
        std::string{DocumentSourceChangeStream::kRegexAllDBs} + "\\." + "news" + "$";

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(NOR(BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                          BSON(OR(BSON("o.to" << BSON("$regex" << secondRegexNs)),
                                  BSON("o.to" << BSON("$regex" << firstRegexNs)))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithInRegexExpressionOnDb) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'to.db': {$in: [/^test.*$/, /^news$/]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                               BSON(OR(fromjson(getNsDbRegexMatchExpr("$o.to", R"(^test.*$)")),
                                       fromjson(getNsDbRegexMatchExpr("$o.to", R"(^news$)")))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithNinRegexExpressionOnDb) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'to.db': {$nin: [/^test.*$/, /^news$/]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(NOR(BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                          BSON(OR(fromjson(getNsDbRegexMatchExpr("$o.to", R"(^test.*$)")),
                                  fromjson(getNsDbRegexMatchExpr("$o.to", R"(^news$)")))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithInRegexExpressionOnCollection) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'to.coll': {$in: [/^test.*$/, /^news$/]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                               BSON(OR(fromjson(getNsCollRegexMatchExpr("$o.to", R"(^test.*$)")),
                                       fromjson(getNsCollRegexMatchExpr("$o.to", R"(^news$)")))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithNinRegexExpressionOnCollection) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'to.coll': {$nin: [/^test.*$/, /^news$/]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(NOR(BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                          BSON(OR(fromjson(getNsCollRegexMatchExpr("$o.to", R"(^test.*$)")),
                                  fromjson(getNsCollRegexMatchExpr("$o.to", R"(^news$)")))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithInExpressionOnDbWithRegexAndString) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'to.db': {$in: [/^test.*$/, 'news']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT(rewrittenMatchExpression);

    const std::string secondRegexNs = std::string("^") + "news" + "\\." +
        std::string{DocumentSourceChangeStream::kRegexAllCollections};

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                               BSON(OR(BSON("o.to" << BSON("$regex" << secondRegexNs)),
                                       fromjson(getNsDbRegexMatchExpr("$o.to", R"(^test.*$)")))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithNinExpressionOnDbWithRegexAndString) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'to.db': {$nin: [/^test.*$/, 'news']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT(rewrittenMatchExpression);

    const std::string secondRegexNs = std::string("^") + "news" + "\\." +
        std::string{DocumentSourceChangeStream::kRegexAllCollections};

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(NOR(BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                          BSON(OR(BSON("o.to" << BSON("$regex" << secondRegexNs)),
                                  fromjson(getNsDbRegexMatchExpr("$o.to", R"(^test.*$)")))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithInExpressionOnCollectionWithRegexAndString) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'to.coll': {$in: [/^test.*$/, 'news']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT(rewrittenMatchExpression);

    const std::string secondRegexNs =
        DocumentSourceChangeStream::kRegexAllDBs + "\\." + "news" + "$";

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                 BSON(OR(BSON("o.to" << BSON("$regex" << secondRegexNs)),
                         fromjson(getNsCollRegexMatchExpr("$o.to", R"(^test.*$)")))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithNinExpressionOnCollectionWithRegexAndString) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'to.coll': {$nin: [/^test.*$/, 'news']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT(rewrittenMatchExpression);

    const std::string secondRegexNs =
        DocumentSourceChangeStream::kRegexAllDBs + "\\." + "news" + "$";

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(NOR(BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                          BSON(OR(BSON("o.to" << BSON("$regex" << secondRegexNs)),
                                  fromjson(getNsCollRegexMatchExpr("$o.to", R"(^test.*$)")))))))));
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteToWithInExpressionOnInvalidDb) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'to.db': {$in: ['test', 1]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT_FALSE(rewrittenMatchExpression);
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteToWithNinExpressionOnInvalidDb) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'to.db': {$nin: ['test', 1]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT_FALSE(rewrittenMatchExpression);
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteToWithInExpressionOnInvalidCollection) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'to.coll': {$in: ['coll', 1]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT_FALSE(rewrittenMatchExpression);
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteToWithNinExpressionOnInvalidCollection) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'to.coll': {$nin: ['coll', 1]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT_FALSE(rewrittenMatchExpression);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithEmptyInExpression) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'to.db': {$in: []}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(AND(fromjson("{op: {$eq: 'c'}}"), fromjson("{$alwaysFalse: 1 }"))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithEmptyNinExpression) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'to.db': {$nin: []}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(NOR(BSON(AND(fromjson("{op: {$eq: 'c'}}"), fromjson("{$alwaysFalse: 1 }"))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithExprOnFullObject) {
    auto expCtx = getExpCtx();
    auto expr =
        fromjson("{$expr: {$eq: ['$to', {db: '" + expCtx->getNamespaceString().db_forTest() +
                 "', coll: '" + expCtx->getNamespaceString().coll() + "'}]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT(rewrittenMatchExpression);

    auto expectedExpr = fromjson(
        "{"
        "  $expr: {"
        "    $eq: [{"
        "      $cond: ["
        "        {"
        "          $and: ["
        "            {$eq: ['$op', {$const: 'c'}]},"
        "            {$ne: ['$o.to', '$$REMOVE']}]"
        "        },"
        "        {"
        "          db: {$substrBytes: ["
        "            '$o.to',"
        "            {$const: 0},"
        "            {$indexOfBytes: ['$o.to', {$const: '.'}]}]},"
        "          coll: {$substrBytes: ["
        "            '$o.to',"
        "            {$add: [{$indexOfBytes: ['$o.to', {$const: '.'}]}, {$const: 1}]},"
        "            {$const: -1}]}"
        "        },"
        "        '$$REMOVE']},"
        "      {db: {$const: 'unittests'}, coll: {$const: 'pipeline_test'}}]}}");

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, expectedExpr);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithExprOnDbFieldPath) {
    auto expCtx = getExpCtx();
    auto expr =
        fromjson("{$expr: {$eq: ['$to.db', '" + expCtx->getNamespaceString().db_forTest() + "']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT(rewrittenMatchExpression);

    auto expectedExpr = fromjson(
        "{"
        "  $expr: {"
        "    $eq: [{"
        "      $cond: ["
        "        {"
        "          $and: ["
        "            {$eq: ['$op', {$const: 'c'}]},"
        "            {$ne: ['$o.to', '$$REMOVE']}]"
        "        },"
        "        {"
        "          $substrBytes: ["
        "            '$o.to',"
        "            {$const: 0},"
        "            {$indexOfBytes: ['$o.to', {$const: '.'}]}]"
        "        },"
        "        '$$REMOVE']},"
        "      {$const: 'unittests'}]}}");

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, expectedExpr);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithExprOnCollFieldPath) {
    auto expCtx = getExpCtx();
    auto expr =
        fromjson("{$expr: {$eq: ['$to.coll', '" + expCtx->getNamespaceString().coll() + "']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT(rewrittenMatchExpression);

    auto expectedExpr = fromjson(
        "{"
        "  $expr: {"
        "    $eq: [{"
        "      $cond: ["
        "        {"
        "          $and: ["
        "            {$eq: ['$op', {$const: 'c'}]},"
        "            {$ne: ['$o.to', '$$REMOVE']}]"
        "        },"
        "        {"
        "          $substrBytes: ["
        "            '$o.to',"
        "            {$add: [{$indexOfBytes: ['$o.to', {$const: '.'}]}, {$const: 1}]},"
        "            {$const: -1}]"
        "        },"
        "        '$$REMOVE']},"
        "      {$const: 'pipeline_test'}]}}");

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, expectedExpr);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithExprOnInvalidFieldPath) {
    auto expCtx = getExpCtx();
    auto expr =
        fromjson("{$expr: {$eq: ['$to.test', '" + expCtx->getNamespaceString().coll() + "']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$expr: {$eq: ['$$REMOVE', {$const: 'pipeline_test'}]}}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithExprOnInvalidDbSubFieldPath) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{$expr: {$eq: ['$to.db.test', '" +
                         expCtx->getNamespaceString().db_forTest() + "']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$expr: {$eq: ['$$REMOVE', {$const: 'unittests'}]}}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithExprOnInvalidCollSubFieldPath) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{$expr: {$eq: ['$to.coll.test', '" + expCtx->getNamespaceString().coll() +
                         "']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$expr: {$eq: ['$$REMOVE', {$const: 'pipeline_test'}]}}"));
}

//
// 'updateDescription' rewrites
//
TEST_F(ChangeStreamRewriteTest, CanInexactlyRewritePredicateOnFieldUpdateDescription) {
    auto expCtx = getExpCtx();
    auto expr = fromjson(
        "{updateDescription: {updatedFields: {}, removedFields: [], truncatedArrays: []}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    // This produces a minimally selective filter which returns all non-replacement update events.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: ["
                               "  {op: {$eq: 'u'}},"
                               "  {'o._id': {$not: {$exists: true}}}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CannotExactlyRewritePredicateOnFieldUpdateDescription) {
    auto expCtx = getExpCtx();
    auto expr = fromjson(
        "{updateDescription: {$ne: {updatedFields: {}, removedFields: [], truncatedArrays: []}}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"updateDescription"});

    ASSERT(rewrittenMatchExpression == nullptr);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteEqNullPredicateOnFieldUpdateDescription) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{updateDescription: {$eq: null}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    // Note that this produces an {$alwaysFalse:1} predicate for update events. This will optimize
    // away the enclosing $and so that only non-updates will be returned from the oplog scan.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$or: ["
                               "  {$and: ["
                               "    {op: {$eq: 'u'}},"
                               "    {'o._id': {$not: {$exists: true}}},"
                               "    {$alwaysFalse: 1}"
                               "  ]},"
                               "  {$or: ["
                               "    {$and: ["
                               "      {op: {$eq: 'u'}},"
                               "      {'o._id': {$exists: true}}"
                               "    ]},"
                               "    {op: {$not: {$eq: 'u'}}}"
                               "  ]}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteExistsPredicateOnFieldUpdateDescription) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{updateDescription: {$exists: true}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    // Note that the {$alwaysTrue:1} predicate is an artefact of the rewrite process. It will be
    // optimized away and will have no functional impact on the filter.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: ["
                               "  {op: {$eq: 'u'}},"
                               "  {'o._id': {$not: {$exists: true}}},"
                               "  {$alwaysTrue: 1}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CanInexactlyRewritePredicateOnFieldUpdateDescriptionUpdatedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.updatedFields': {}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    // This produces a minimally selective filter which returns all non-replacement update events.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: ["
                               "  {op: {$eq: 'u'}},"
                               "  {'o._id': {$not: {$exists: true}}}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest,
       CannotExactlyRewritePredicateOnFieldUpdateDescriptionUpdatedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.updatedFields': {$ne: {}}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"updateDescription"});

    ASSERT(rewrittenMatchExpression == nullptr);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteEqNullPredicateOnFieldUpdateDescriptionUpdatedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.updatedFields': {$eq: null}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    // Note that this produces an {$alwaysFalse:1} predicate for update events. This will optimize
    // away the enclosing $and so that only non-updates will be returned from the oplog scan.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$or: ["
                               "  {$and: ["
                               "    {op: {$eq: 'u'}},"
                               "    {'o._id': {$not: {$exists: true}}},"
                               "    {$alwaysFalse: 1}"
                               "  ]},"
                               "  {$or: ["
                               "    {$and: ["
                               "      {op: {$eq: 'u'}},"
                               "      {'o._id': {$exists: true}}"
                               "    ]},"
                               "    {op: {$not: {$eq: 'u'}}}"
                               "  ]}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteExistsPredicateOnFieldUpdateDescriptionUpdatedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.updatedFields': {$exists: true}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    // Note that the {$alwaysTrue:1} predicate is an artefact of the rewrite process. It will be
    // optimized away and will have no functional impact on the filter.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: ["
                               "  {op: {$eq: 'u'}},"
                               "  {'o._id': {$not: {$exists: true}}},"
                               "  {$alwaysTrue: 1}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest,
       CanRewriteArbitraryPredicateOnFieldUpdateDescriptionUpdatedFieldsFoo) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.updatedFields.foo': {$lt: 'b'}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: ["
                               "  {op: {$eq: 'u'}},"
                               "  {'o._id': {$not: {$exists: true}}},"
                               "  {$or: ["
                               "    {'o.diff.i.foo': {$lt: 'b'}},"
                               "    {'o.diff.u.foo': {$lt: 'b'}},"
                               "    {'o.$set.foo': {$lt: 'b'}}"
                               "  ]}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteEqNullPredicateOnFieldUpdateDescriptionUpdatedFieldsFoo) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.updatedFields.foo': {$eq: null}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    // Note that we perform an $and of all three oplog locations for this rewrite.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$or: ["
                               "  {$and: ["
                               "    {op: {$eq: 'u'}},"
                               "    {'o._id': {$not: {$exists: true}}},"
                               "    {$and: ["
                               "      {'o.diff.i.foo': {$eq: null}},"
                               "      {'o.diff.u.foo': {$eq: null}},"
                               "      {'o.$set.foo': {$eq: null}}"
                               "    ]}"
                               "  ]},"
                               "  {$or: ["
                               "    {$and: ["
                               "      {op: {$eq: 'u'}},"
                               "      {'o._id': {$exists: true}}"
                               "    ]},"
                               "    {op: {$not: {$eq: 'u'}}}"
                               "  ]}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest,
       CanInexactlyRewriteEqPredicateOnFieldUpdateDescriptionUpdatedFieldsFooBar) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.updatedFields.foo.bar': 'b'}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    // This produces a minimally selective filter which returns all non-replacement update events.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: ["
                               "  {op: {$eq: 'u'}},"
                               "  {'o._id': {$not: {$exists: true}}}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest,
       CannotExactlyRewriteEqPredicateOnFieldUpdateDescriptionUpdatedFieldsFooBar) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.updatedFields.foo.bar': {$ne: 'b'}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"updateDescription"});

    ASSERT(rewrittenMatchExpression == nullptr);
}

TEST_F(ChangeStreamRewriteTest,
       CannotRewriteEqNullPredicateOnFieldUpdateDescriptionUpdatedFieldsFooBar) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.updatedFields.foo.bar': {$eq: null}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"updateDescription"});

    ASSERT(rewrittenMatchExpression == nullptr);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteStringEqPredicateOnFieldUpdateDescriptionRemovedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.removedFields': 'z'}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: ["
                               "  {op: {$eq: 'u'}},"
                               "  {'o._id': {$not: {$exists: true}}},"
                               "  {$or: ["
                               "    {'o.diff.d.z': {$exists: true}},"
                               "    {'o.$unset.z': {$exists: true}}"
                               "  ]}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteEqNullPredicateOnFieldUpdateDescriptionRemovedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.removedFields': {$eq: null}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    // Note that this produces an {$alwaysFalse:1} predicate for update events. This will optimize
    // away the enclosing $and so that only non-updates will be returned from the oplog scan.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$or: ["
                               "  {$and: ["
                               "    {op: {$eq: 'u'}},"
                               "    {'o._id': {$not: {$exists: true}}},"
                               "    {$alwaysFalse: 1}"
                               "  ]},"
                               "  {$or: ["
                               "    {$and: ["
                               "      {op: {$eq: 'u'}},"
                               "      {'o._id': {$exists: true}}"
                               "    ]},"
                               "    {op: {$not: {$eq: 'u'}}}"
                               "  ]}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteExistsPredicateOnFieldUpdateDescriptionRemovedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.removedFields': {$exists: true}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    // Note that the {$alwaysTrue:1} predicate is an artefact of the rewrite process. It will be
    // optimized away and will have no functional impact on the filter.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: ["
                               "  {op: {$eq: 'u'}},"
                               "  {'o._id': {$not: {$exists: true}}},"
                               "  {$alwaysTrue: 1}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest,
       CanInexactlyRewriteDottedStringEqPredicateOnFieldUpdateDescriptionRemovedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.removedFields': 'u.v'}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    // This produces a minimally selective filter which returns all non-replacement update events.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: ["
                               "  {op: {$eq: 'u'}},"
                               "  {'o._id': {$not: {$exists: true}}}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest,
       CannotExactlyRewriteDottedStringEqPredicateOnFieldUpdateDescriptionRemovedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.removedFields': {$ne: 'u.v'}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"updateDescription"});

    ASSERT(rewrittenMatchExpression == nullptr);
}

TEST_F(ChangeStreamRewriteTest,
       CanInexactlyRewriteNonStringEqPredicateOnFieldUpdateDescriptionRemovedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.removedFields': ['z']}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    // This produces a minimally selective filter which returns all non-replacement update events.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: ["
                               "  {op: {$eq: 'u'}},"
                               "  {'o._id': {$not: {$exists: true}}}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest,
       CannotExactlyRewriteNonStringEqPredicateOnFieldUpdateDescriptionRemovedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.removedFields': {$ne: ['z']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"updateDescription"});

    ASSERT(rewrittenMatchExpression == nullptr);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteStringInPredicateOnFieldUpdateDescriptionRemovedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.removedFields': {$in: ['w', 'y']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: ["
                               "  {op: {$eq: 'u'}},"
                               "  {'o._id': {$not: {$exists: true}}},"
                               "  {$or: ["
                               "    {$or: ["
                               "        {'o.diff.d.w': {$exists: true}},"
                               "        {'o.$unset.w': {$exists: true}}"
                               "    ]},"
                               "    {$or: ["
                               "        {'o.diff.d.y': {$exists: true}},"
                               "        {'o.$unset.y': {$exists: true}}"
                               "    ]}"
                               "  ]}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteEmptyInPredicateOnFieldUpdateDescriptionRemovedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.removedFields': {$in: []}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: ["
                               "  {op: {$eq: 'u'}},"
                               "  {'o._id': {$not: {$exists: true}}},"
                               "  {$alwaysFalse: 1}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest,
       CanInexactlyRewriteNonStringInPredicateOnFieldUpdateDescriptionRemovedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.removedFields': {$in: ['w', ['y']]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    // This produces a minimally selective filter which returns all non-replacement update events.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: ["
                               "  {op: {$eq: 'u'}},"
                               "  {'o._id': {$not: {$exists: true}}}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest,
       CannotExactlyRewriteNonStringInPredicateOnFieldUpdateDescriptionRemovedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.removedFields': {$nin: ['w', ['y']]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"updateDescription"});

    ASSERT(rewrittenMatchExpression == nullptr);
}

TEST_F(ChangeStreamRewriteTest,
       CanInexactlyRewriteRegexInPredicateOnFieldUpdateDescriptionRemovedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.removedFields': {$in: [/ab*c/, /de*f/]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    // This produces a minimally selective filter which returns all non-replacement update events.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: ["
                               "  {op: {$eq: 'u'}},"
                               "  {'o._id': {$not: {$exists: true}}}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest,
       CannotExactlyRewriteRegexInPredicateOnFieldUpdateDescriptionRemovedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.removedFields': {$nin: [/ab*c/, /de*f/]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"updateDescription"});

    ASSERT(rewrittenMatchExpression == nullptr);
}

TEST_F(ChangeStreamRewriteTest,
       CanInexactlyRewriteArbitraryPredicateOnFieldUpdateDescriptionRemovedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.removedFields': {$lt: 'z'}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    // This produces a minimally selective filter which returns all non-replacement update events.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: ["
                               "  {op: {$eq: 'u'}},"
                               "  {'o._id': {$not: {$exists: true}}}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest,
       CannotExactlyRewriteArbitraryPredicateOnFieldUpdateDescriptionRemovedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.removedFields': {$not: {$lt: 'z'}}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"updateDescription"});

    ASSERT(rewrittenMatchExpression == nullptr);
}

TEST_F(ChangeStreamRewriteTest,
       CanInexactlyRewriteArbitraryPredicateOnFieldUpdateDescriptionRemovedFieldsFoo) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.removedFields.foo': {$lt: 'z'}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    // This produces a minimally selective filter which returns all non-replacement update events.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: ["
                               "  {op: {$eq: 'u'}},"
                               "  {'o._id': {$not: {$exists: true}}}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest,
       CannotExactlyRewriteArbitraryPredicateOnFieldUpdateDescriptionRemovedFieldsFoo) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.removedFields.foo': {$not: {$lt: 'z'}}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"updateDescription"});

    ASSERT(rewrittenMatchExpression == nullptr);
}

TEST_F(ChangeStreamRewriteTest,
       CanInexactlyRewritePredicateOnFieldUpdateDescriptionTruncatedArrays) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.truncatedArrays': []}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    // This produces a minimally selective filter which returns all non-replacement update events.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: ["
                               "  {op: {$eq: 'u'}},"
                               "  {'o._id': {$not: {$exists: true}}}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest,
       CannotExactlyRewritePredicateOnFieldUpdateDescriptionTruncatedArrays) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.truncatedArrays': {$ne: []}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto bsonObjsArray = std::vector<BSONObj>{};
    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), bsonObjsArray, {"updateDescription"});

    ASSERT(rewrittenMatchExpression == nullptr);
}

}  // namespace
}  // namespace mongo
