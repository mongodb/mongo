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

#include <memory>
#include <string>
#include <utility>

#include <absl/container/node_hash_map.h>

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/abt/canonical_query_translation.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/metadata.h"
#include "mongo/db/query/optimizer/metadata_factory.h"
#include "mongo/db/query/optimizer/node.h"  // IWYU pragma: keep
#include "mongo/db/query/optimizer/opt_phase_manager.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"
#include "mongo/db/query/optimizer/utils/unit_test_pipeline_utils.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/db/query/optimizer/utils/utils.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo::optimizer {

namespace {

using ABTTranslationTest = ABTGoldenTestFixture;

TEST_F(ABTTranslationTest, EqTranslation) {
    testABTTranslationAndOptimization("$match basic", "[{$match: {a: 1}}]");

    testABTTranslationAndOptimization("$match with $expr $eq with dotted path",
                                      "[{$match: {$expr: {$eq: ['$a.b', 1]}}}]");

    testABTTranslationAndOptimization("$match with $expr $eq",
                                      "[{$match: {$expr: {$eq: ['$a', 1]}}}]");
}

TEST_F(ABTTranslationTest, SizeTranslation) {
    testABTTranslationAndOptimization("$match with $size", "[{$match: {'a': {$size: 1}}}]");
}

TEST_F(ABTTranslationTest, InequalityTranslation) {
    testABTTranslationAndOptimization("$match with range", "[{$match: {'a': {$gt: 70}}}]");

    testABTTranslationAndOptimization("$match with range conjunction",
                                      "[{$match: {'a': {$gt: 70, $lt: 90}}}]");
}

TEST_F(ABTTranslationTest, InTranslation) {
    testABTTranslationAndOptimization("$match with $in, empty list", "[{$match: {a: {$in: []}}}]");

    testABTTranslationAndOptimization("match with $in, singleton list",
                                      "[{$match: {a: {$in: [1]}}}]");

    testABTTranslationAndOptimization(
        "$match with $in and a list of equalities becomes a comparison to an EqMember list.",
        "[{$match: {a: {$in: [1, 2, 3]}}}]");

    testABTTranslationAndOptimization("match with $in over an array, duplicated equalities removed",
                                      "[{$match: {a: {$in: ['abc', 'def', 'ghi', 'def']}}}]");
}

TEST_F(ABTTranslationTest, AndOrTranslation) {
    testABTTranslationAndOptimization("$match conjunction", "[{$match: {$and: [{a: 1}, {b: 2}]}}]");

    testABTTranslationAndOptimization("$match disjunction", "[{$match: {$or: [{a: 1}, {b: 2}]}}]");

    testABTTranslationAndOptimization("$match nested conjunction",
                                      "[{$match: {$and: [{$and: [{a: 1}, {b: 2}]}, {c: 3}]}}]");
}

TEST_F(ABTTranslationTest, ElemMatchTranslation) {
    testABTTranslationAndOptimization("$match with empty $elemMatch",
                                      "[{$match: {'a': {$elemMatch: {}}}}]");

    testABTTranslationAndOptimization(
        "ensure the PathGet and PathTraverse operators interact correctly when $in is "
        "under $elemMatch",
        "[{$match: {'a.b': {$elemMatch: {$in: [1, 2, 3]}}}}]");

    testABTTranslationAndOptimization(
        "$match with value $elemMatch: observe type bracketing in the filter.",
        "[{$project: {a: {$literal: [1, 2, 3, 4]}}}, {$match: {a: {$elemMatch: {$gte: 2, $lte: "
        "3}}}}]");

    testABTTranslationAndOptimization("$match object $elemMatch",
                                      "[{$match: {'a': {$elemMatch: {'b': {$eq: 5}}}}}]");
}

TEST_F(ABTTranslationTest, ExistsTranslation) {
    testABTTranslationAndOptimization("$match exists", "[{$match: {a: {$exists: true}}}]");

    testABTTranslationAndOptimization("$match exists", "[{$match: {a: {$exists: false}}}]");

    testABTTranslationAndOptimization("$match exists", "[{$match: {'a.b': {$exists: true}}}]");
}

TEST_F(ABTTranslationTest, NotTranslation) {
    testABTTranslationAndOptimization("$match not", "[{$match: {a: {$not: {$exists: true}}}}]");

    testABTTranslationAndOptimization("$match not", "[{$match: {a: {$not: {$eq: 5}}}}]");

    testABTTranslationAndOptimization("$match with $ne", "[{$match: {'a': {$ne: 2}}}]");
}

TEST_F(ABTTranslationTest, SimpleProjectionTranslation) {
    testABTTranslationAndOptimization("inclusion project",
                                      "[{$project: {a1: 1, a2: 1, a3: 1, a4: 1, a5: 1, a6: 1}}]");

    testABTTranslationAndOptimization("inclusion project dotted paths",
                                      "[{$project: {'a.b':1, 'a.c':1, 'b':1}}]");

    testABTTranslationAndOptimization("exclusion project", "[{$project: {a: 0, b: 0}}]");

    testABTTranslationAndOptimization("$project with deeply nested path",
                                      "[{$project: {'a1.b.c':1, 'a.b.c.d.e':'str'}}]");
}

TEST_F(ABTTranslationTest, ComputedProjectionTranslation) {
    testABTTranslationAndOptimization(
        "project rename through addFields: since '$z' is a single element, it will be "
        "considered a renamed path",
        "[{$addFields: {a: '$z'}}]");

    testABTTranslationAndOptimization(
        "project rename: since '$c' is a single element, it will be considered a renamed path",
        "[{$project: {'a.b': '$c'}}]");

    testABTTranslationAndOptimization("project rename dotted paths",
                                      "[{$project: {'a.b.c': '$x.y.z'}}]");

    testABTTranslationAndOptimization("inclusion project with computed field",
                                      "[{$project: {a: {$add: ['$c.d', 2]}, b: 1}}]");

    testABTTranslationAndOptimization("replaceRoot", "[{$replaceRoot: {newRoot: '$a'}}]");

    testABTTranslationAndOptimization("$project with computed array",
                                      "[{$project: {a: ['$b', '$c']}}]");
}

TEST_F(ABTTranslationTest, GroupTranslation) {
    testABTTranslationAndOptimization(
        "$project then $match",
        "[{$project: {s: {$add: ['$a', '$b']}, c: 1}}, {$match: {$or: [{c: 2}, {s: {$gte: "
        "10}}]}}]");


    testABTTranslationAndOptimization(
        "basic $group", "[{$group: {_id: '$a.b', s: {$sum: {$multiply: ['$b', '$c']}}}}]");

    testABTTranslationAndOptimization("$group local global",
                                      "[{$group: {_id: '$a', c: {$sum: '$b'}}}]");

    testABTTranslationAndOptimization("$group with complex _id",
                                      "[{$group: {_id: {'isin': '$isin', 'year': '$year'}, "
                                      "'count': {$sum: 1}, 'open': {$first: "
                                      "'$$ROOT'}}}]");
}

TEST_F(ABTTranslationTest, UnwindTranslation) {
    testABTTranslationAndOptimization("basic $unwind", "[{$unwind: {path: '$a.b.c'}}]");

    testABTTranslationAndOptimization(
        "complex $unwind",
        "[{$unwind: {path: '$a.b.c', includeArrayIndex: 'p1.pid', preserveNullAndEmptyArrays: "
        "true}}]");

    testABTTranslationAndOptimization(
        "$unwind with $group",
        "[{$unwind:{path: '$a.b', preserveNullAndEmptyArrays: true}}, {$group:{_id: '$a.b'}}]");
}

TEST_F(ABTTranslationTest, UnionTranslation) {
    std::string scanDefA = "collA";
    std::string scanDefB = "collB";
    Metadata metadataUnion{{{scanDefA, {}}, {scanDefB, {}}}};
    testABTTranslationAndOptimization(
        "union",
        "[{$unionWith: 'collB'}, {$match: {_id: 1}}]",
        scanDefA,
        {},
        metadataUnion,
        {},
        false,
        {{NamespaceString::createNamespaceString_forTest("a." + scanDefB), {}}});
}

TEST_F(ABTTranslationTest, SortTranslation) {
    testABTTranslationAndOptimization("sort limit skip",
                                      "[{$limit: 5}, {$skip: 3}, {$sort: {a: 1, b: -1}}]");

    testABTTranslationAndOptimization("$match sort", "[{$match: {'a': 10}}, {$sort: {'a': 1}}]");
}

TEST_F(ServiceContextTest, BasicCanonicalQueryTranslation) {
    Metadata metadata({{"collection", createScanDef({}, {})}});

    auto opCtx = makeOperationContext();
    auto findCommand = query_request_helper::makeFromFindCommandForTests(
        fromjson("{find: 'collection', '$db': 'test', filter: {a: 10, b: 20, c:30}}"));
    auto cq = std::make_unique<CanonicalQuery>(
        CanonicalQueryParams{.expCtx = makeExpressionContext(opCtx.get(), *findCommand),
                             .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});
    auto prefixId = PrefixId::createForTests();

    MatchExpression::unparameterize(cq->getPrimaryMatchExpression());

    {
        QueryParameterMap qp;
        auto translation = translateCanonicalQueryToABT(
            metadata, *cq, ProjectionName{"test"}, make<ScanNode>("test", "test"), prefixId, qp);
        ASSERT_EXPLAIN_V2_AUTO(
            "Root [{test}]\n"
            "Filter []\n"
            "|   EvalFilter []\n"
            "|   |   Variable [test]\n"
            "|   PathGet [b]\n"
            "|   PathTraverse [1]\n"
            "|   PathCompare [Eq]\n"
            "|   Const [20]\n"
            "Filter []\n"
            "|   EvalFilter []\n"
            "|   |   Variable [test]\n"
            "|   PathGet [a]\n"
            "|   PathTraverse [1]\n"
            "|   PathCompare [Eq]\n"
            "|   Const [10]\n"
            "Filter []\n"
            "|   EvalFilter []\n"
            "|   |   Variable [test]\n"
            "|   PathGet [c]\n"
            "|   PathTraverse [1]\n"
            "|   PathCompare [Eq]\n"
            "|   Const [30]\n"
            "Scan [test, {test}]\n",
            translation);
    }

    {
        // Repeat translation to verify case where we generate a single output FilterNode.
        QueryParameterMap qp;
        auto translation = translateCanonicalQueryToABT(
            metadata, *cq, ProjectionName{"test"}, make<ScanNode>("test", "test"), prefixId, qp, 1);
        ASSERT_EXPLAIN_V2_AUTO(
            "Root [{test}]\n"
            "Filter []\n"
            "|   EvalFilter []\n"
            "|   |   Variable [test]\n"
            "|   PathComposeM []\n"
            "|   |   PathGet [b]\n"
            "|   |   PathTraverse [1]\n"
            "|   |   PathCompare [Eq]\n"
            "|   |   Const [20]\n"
            "|   PathComposeM []\n"
            "|   |   PathGet [a]\n"
            "|   |   PathTraverse [1]\n"
            "|   |   PathCompare [Eq]\n"
            "|   |   Const [10]\n"
            "|   PathGet [c]\n"
            "|   PathTraverse [1]\n"
            "|   PathCompare [Eq]\n"
            "|   Const [30]\n"
            "Scan [test, {test}]\n",
            translation);
    }
}

TEST_F(ServiceContextTest, NonDescriptiveNames) {
    auto prefixId = PrefixId::create(false /*useDescriptiveVarNames*/);
    Metadata metadata({{"collection", createScanDef({}, {})}});

    const std::string& pipeline = "[{$group: {_id: '$a.b', s: {$sum: {$multiply: ['$b', '$c']}}}}]";
    const ProjectionName scanProjName = prefixId.getNextId("scan");

    ABT translated =
        translatePipeline(metadata, pipeline, scanProjName, "collection", prefixId, {});

    // Observe projection names are not descriptive. They are of the form "pXXXX".
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{p4}]\n"
        "Evaluation [{p4}]\n"
        "|   EvalPath []\n"
        "|   |   Const [{}]\n"
        "|   PathComposeM []\n"
        "|   |   PathField [s]\n"
        "|   |   PathConstant []\n"
        "|   |   Variable [p2]\n"
        "|   PathField [_id]\n"
        "|   PathConstant []\n"
        "|   Variable [p1]\n"
        "GroupBy [{p1}]\n"
        "|   aggregations: \n"
        "|       [p2]\n"
        "|           FunctionCall [$sum]\n"
        "|           Variable [p3]\n"
        "Evaluation [{p3}]\n"
        "|   BinaryOp [Mult]\n"
        "|   |   EvalPath []\n"
        "|   |   |   Variable [p0]\n"
        "|   |   PathGet [c]\n"
        "|   |   PathIdentity []\n"
        "|   EvalPath []\n"
        "|   |   Variable [p0]\n"
        "|   PathGet [b]\n"
        "|   PathIdentity []\n"
        "Evaluation [{p1}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [p0]\n"
        "|   PathGet [a]\n"
        "|   PathTraverse [inf]\n"
        "|   PathGet [b]\n"
        "|   PathIdentity []\n"
        "Scan [collection, {p0}]\n",
        translated);
}

TEST_F(ABTTranslationTest, ParameterizedEqTranslation) {
    testParameterizedABTTranslation("FIND - $match basic",
                                    "{find: 'collection', '$db': 'test', filter: {a: 1}}");
    testParameterizedABTTranslation("PIPELINE - $match basic", "", "[{$match: {'a': 1}}]");
}

TEST_F(ABTTranslationTest, ParameterizedInequalityTranslation) {
    testParameterizedABTTranslation("FIND - $match with range",
                                    "{find: 'collection', '$db': 'test', filter: {a: {$gte: 70}}}");

    testParameterizedABTTranslation(
        "PIPELINE - $match with range", "", "[{$match: {'a': {$gte: 70}}}]");

    testParameterizedABTTranslation(
        "FIND - $match with range conjunction",
        "{find: 'collection', '$db': 'test', filter: {a: {$gt: 70, $lt: 90}}}");

    testParameterizedABTTranslation(
        "PIPELINE - $match with range conjunction", "", "[{$match: {'a': {$gt: 70, $lt: 90}}}]");

    testParameterizedABTTranslation(
        "FIND - $match with range conjunction on string data",
        "{find: 'collection', '$db': 'test', filter: {a: {$gt: 'a', $lt: 'd'}}}");

    testParameterizedABTTranslation("PIPELINE - $match with range conjunction on string data",
                                    "",
                                    "[{$match: {'a': {$gt: 'a', $lt: 'd'}}}]");

    testParameterizedABTTranslation(
        "FIND - $match with range on double",
        "{find: 'collection', '$db': 'test', filter: {a: {$gte: 4.5}}}");

    testParameterizedABTTranslation(
        "PIPELINE - $match with range on double", "", "[{$match: {'a': {$gte: 4.5}}}]");
}

TEST_F(ABTTranslationTest, ParameterizedInTranslation) {
    testParameterizedABTTranslation("FIND - $match with $in, empty list",
                                    "{find: 'collection', '$db': 'test', filter: {a: {$in: []}}}");

    testParameterizedABTTranslation(
        "PIPELINE - $match with $in, empty list", "", "[{$match: {'a': {$in: []}}}]");

    testParameterizedABTTranslation("FIND - $match with $in, singleton list",
                                    "{find: 'collection', '$db': 'test', filter: {a: {$in: [1]}}}");

    testParameterizedABTTranslation(
        "PIPELINE - $match with $in, singleton list", "", "[{$match: {'a': {$in: [1]}}}]");

    testParameterizedABTTranslation(
        "FIND - $match with $in and a list of equalities becomes a comparison to an EqMember list",
        "{find: 'collection', '$db': 'test', filter: {a: {$in: [1, 2, 3]}}}");

    testParameterizedABTTranslation(
        "PIPELINE - $match with $in and a list of equalities becomes a comparison to an EqMember "
        "list",
        "",
        "[{$match: {'a': {$in: [1, 2, 3]}}}]");

    testParameterizedABTTranslation(
        "FIND - $match with $in over an array, duplicated equalities removed",
        "{find: 'collection', '$db': 'test', filter: {a: {$in: ['abc', 'def', 'ghi', 'def']}}}");

    testParameterizedABTTranslation(
        "PIPELINE - $match with $in over an array, duplicated equalities removed",
        "",
        "[{$match: {'a': {$in: ['abc', 'def', 'ghi', 'def']}}}]");
}

TEST_F(ABTTranslationTest, ParameterizedSizeTranslation) {
    testParameterizedABTTranslation("FIND - $match with $size",
                                    "{find: 'collection', '$db': 'test', filter: {a: {$size: 2}}}");

    testParameterizedABTTranslation(
        "PIPELINE - $match with $size", "", "[{$match: {'a': {$size: 2}}}]");
}
}  // namespace
}  // namespace mongo::optimizer
