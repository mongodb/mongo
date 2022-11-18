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

#include "mongo/db/pipeline/abt/utils.h"
#include "mongo/db/query/optimizer/utils/unit_test_pipeline_utils.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::optimizer {

namespace {

unittest::GoldenTestConfig goldenTestConfigABTTranslation{"src/mongo/db/test_output/pipeline/abt"};

class ABTTranslationTest : public unittest::Test {
public:
    ABTTranslationTest()
        : gctx(std::make_unique<unittest::GoldenTestContext>(&goldenTestConfigABTTranslation)) {}

protected:
    void testTranslation(const std::string& description, const std::string& pipeline) {
        testABTTranslationAndOptimization(*gctx, description, pipeline);
    }

    std::unique_ptr<unittest::GoldenTestContext> gctx;
};

TEST_F(ABTTranslationTest, EqTranslation) {
    testTranslation("$match basic", "[{$match: {a: 1}}]");

    testTranslation("$match with $expr $eq with dotted path",
                    "[{$match: {$expr: {$eq: ['$a.b', 1]}}}]");

    testTranslation("$match with $expr $eq", "[{$match: {$expr: {$eq: ['$a', 1]}}}]");
}

TEST_F(ABTTranslationTest, InequalityTranslation) {
    testTranslation("$match with range", "[{$match: {'a': {$gt: 70}}}]");

    testTranslation("$match with range conjunction", "[{$match: {'a': {$gt: 70, $lt: 90}}}]");
}

TEST_F(ABTTranslationTest, InTranslation) {
    testTranslation("$match with $in, empty list", "[{$match: {a: {$in: []}}}]");

    testTranslation("match with $in, singleton list", "[{$match: {a: {$in: [1]}}}]");

    testTranslation(
        "$match with $in and a list of equalities becomes a comparison to an EqMember list.",
        "[{$match: {a: {$in: [1, 2, 3]}}}]");

    testTranslation("match with $in over an array, duplicated equalities removed",
                    "[{$match: {a: {$in: ['abc', 'def', 'ghi', 'def']}}}]");
}

TEST_F(ABTTranslationTest, AndOrTranslation) {
    testTranslation("$match conjunction", "[{$match: {$and: [{a: 1}, {b: 2}]}}]");

    testTranslation("$match disjunction", "[{$match: {$or: [{a: 1}, {b: 2}]}}]");

    testTranslation("$match nested conjunction",
                    "[{$match: {$and: [{$and: [{a: 1}, {b: 2}]}, {c: 3}]}}]");
}

TEST_F(ABTTranslationTest, ElemMatchTranslation) {
    testTranslation("$match with empty $elemMatch", "[{$match: {'a': {$elemMatch: {}}}}]");

    testTranslation(
        "ensure the PathGet and PathTraverse operators interact correctly when $in is "
        "under $elemMatch",
        "[{$match: {'a.b': {$elemMatch: {$in: [1, 2, 3]}}}}]");

    testTranslation(
        "$match with value $elemMatch: observe type bracketing in the filter.",
        "[{$project: {a: {$literal: [1, 2, 3, 4]}}}, {$match: {a: {$elemMatch: {$gte: 2, $lte: "
        "3}}}}]");

    testTranslation("$match object $elemMatch", "[{$match: {'a': {$elemMatch: {'b': {$eq: 5}}}}}]");
}

TEST_F(ABTTranslationTest, ExistsTranslation) {
    testTranslation("$match exists", "[{$match: {a: {$exists: true}}}]");

    testTranslation("$match exists", "[{$match: {a: {$exists: false}}}]");

    testTranslation("$match exists", "[{$match: {'a.b': {$exists: true}}}]");
}

TEST_F(ABTTranslationTest, NotTranslation) {
    testTranslation("$match not", "[{$match: {a: {$not: {$exists: true}}}}]");

    testTranslation("$match not", "[{$match: {a: {$not: {$eq: 5}}}}]");

    testTranslation("$match with $ne", "[{$match: {'a': {$ne: 2}}}]");
}

TEST_F(ABTTranslationTest, SimpleProjectionTranslation) {
    testTranslation("inclusion project",
                    "[{$project: {a1: 1, a2: 1, a3: 1, a4: 1, a5: 1, a6: 1}}]");

    testTranslation("inclusion project dotted paths", "[{$project: {'a.b':1, 'a.c':1, 'b':1}}]");

    testTranslation("exclusion project", "[{$project: {a: 0, b: 0}}]");

    testTranslation("$project with deeply nested path",
                    "[{$project: {'a1.b.c':1, 'a.b.c.d.e':'str'}}]");
}

TEST_F(ABTTranslationTest, ComputedProjectionTranslation) {
    testTranslation(
        "project rename through addFields: since '$z' is a single element, it will be "
        "considered a renamed path",
        "[{$addFields: {a: '$z'}}]");

    testTranslation(
        "project rename: since '$c' is a single element, it will be considered a renamed path",
        "[{$project: {'a.b': '$c'}}]");

    testTranslation("project rename dotted paths", "[{$project: {'a.b.c': '$x.y.z'}}]");

    testTranslation("inclusion project with computed field",
                    "[{$project: {a: {$add: ['$c.d', 2]}, b: 1}}]");

    testTranslation("replaceRoot", "[{$replaceRoot: {newRoot: '$a'}}]");

    testTranslation("$project with computed array", "[{$project: {a: ['$b', '$c']}}]");
}

TEST_F(ABTTranslationTest, GroupTranslation) {
    testTranslation(
        "$project then $match",
        "[{$project: {s: {$add: ['$a', '$b']}, c: 1}}, {$match: {$or: [{c: 2}, {s: {$gte: "
        "10}}]}}]");


    testTranslation("basic $group",
                    "[{$group: {_id: '$a.b', s: {$sum: {$multiply: ['$b', '$c']}}}}]");

    testTranslation("$group local global", "[{$group: {_id: '$a', c: {$sum: '$b'}}}]");

    testTranslation("$group with complex _id",
                    "[{$group: {_id: {'isin': '$isin', 'year': '$year'}, "
                    "'count': {$sum: 1}, 'open': {$first: "
                    "'$$ROOT'}}}]");
}

TEST_F(ABTTranslationTest, UnwindTranslation) {
    testTranslation("basic $unwind", "[{$unwind: {path: '$a.b.c'}}]");

    testTranslation(
        "complex $unwind",
        "[{$unwind: {path: '$a.b.c', includeArrayIndex: 'p1.pid', preserveNullAndEmptyArrays: "
        "true}}]");

    testTranslation(
        "$unwind with $group",
        "[{$unwind:{path: '$a.b', preserveNullAndEmptyArrays: true}}, {$group:{_id: '$a.b'}}]");
}

TEST_F(ABTTranslationTest, UnionTranslation) {
    std::string scanDefA = "collA";
    std::string scanDefB = "collB";
    Metadata metadataUnion{{{scanDefA, {}}, {scanDefB, {}}}};
    testABTTranslationAndOptimization(*gctx,
                                      "union",
                                      "[{$unionWith: 'collB'}, {$match: {_id: 1}}]",
                                      scanDefA,
                                      {},
                                      metadataUnion,
                                      {},
                                      false,
                                      {{NamespaceString("a." + scanDefB), {}}});
}

TEST_F(ABTTranslationTest, SortTranslation) {
    testTranslation("sort limit skip", "[{$limit: 5}, {$skip: 3}, {$sort: {a: 1, b: -1}}]");

    testTranslation("$match sort", "[{$match: {'a': 10}}, {$sort: {'a': 1}}]");
}

}  // namespace
}  // namespace mongo::optimizer
