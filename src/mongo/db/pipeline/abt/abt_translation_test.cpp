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


namespace mongo::optimizer {

unittest::GoldenTestConfig goldenTestConfigABTTranslation{"src/mongo/db/test_output/pipeline/abt"};

TEST(ABTTranslate, BasicTests) {
    unittest::GoldenTestContext gctx(&goldenTestConfigABTTranslation);

    testABTTranslationAndOptimization(
        gctx, "$match with $in, empty list", "[{$match: {a: {$in: []}}}]");

    testABTTranslationAndOptimization(
        gctx, "match with $in, singleton list", "[{$match: {a: {$in: [1]}}}]");

    testABTTranslationAndOptimization(
        gctx,
        "$match with $in and a list of equalities becomes a comparison to an EqMember list.",
        "[{$match: {a: {$in: [1, 2, 3]}}}]");

    testABTTranslationAndOptimization(gctx,
                                      "match with $in over an array, duplicated equalities removed",
                                      "[{$match: {a: {$in: ['abc', 'def', 'ghi', 'def']}}}]");


    testABTTranslationAndOptimization(
        gctx, "$match with empty $elemMatch", "[{$match: {'a': {$elemMatch: {}}}}]");

    testABTTranslationAndOptimization(
        gctx,
        "ensure the PathGet and PathTraverse operators interact correctly when $in is "
        "under $elemMatch",
        "[{$match: {'a.b': {$elemMatch: {$in: [1, 2, 3]}}}}]");

    testABTTranslationAndOptimization(
        gctx, "sort limit skip", "[{$limit: 5}, {$skip: 3}, {$sort: {a: 1, b: -1}}]");

    testABTTranslationAndOptimization(
        gctx, "inclusion project", "[{$project: {a1: 1, a2: 1, a3: 1, a4: 1, a5: 1, a6: 1}}]");

    testABTTranslationAndOptimization(
        gctx,
        "project rename through addFields: since '$z' is a single element, it will be "
        "considered a renamed path",
        "[{$addFields: {a: '$z'}}]");

    testABTTranslationAndOptimization(
        gctx,
        "project rename: since '$c' is a single element, it will be considered a renamed path",
        "[{$project: {'a.b': '$c'}}]");

    testABTTranslationAndOptimization(
        gctx, "project rename dotted paths", "[{$project: {'a.b.c': '$x.y.z'}}]");

    testABTTranslationAndOptimization(
        gctx, "inclusion project dotted paths", "[{$project: {'a.b':1, 'a.c':1, 'b':1}}]");

    testABTTranslationAndOptimization(gctx,
                                      "inclusion project with computed field",
                                      "[{$project: {a: {$add: ['$c.d', 2]}, b: 1}}]");

    testABTTranslationAndOptimization(gctx, "exclusion project", "[{$project: {a: 0, b: 0}}]");

    testABTTranslationAndOptimization(gctx, "replaceRoot", "[{$replaceRoot: {newRoot: '$a'}}]");

    testABTTranslationAndOptimization(gctx, "$match basic", "[{$match: {a: 1, b: 2}}]");

    testABTTranslationAndOptimization(
        gctx, "$match with $expr $eq", "[{$match: {$expr: {$eq: ['$a', 1]}}}]");

    testABTTranslationAndOptimization(
        gctx, "$match with $expr $eq with dotted path", "[{$match: {$expr: {$eq: ['$a.b', 1]}}}]");

    testABTTranslationAndOptimization(
        gctx,
        "$match with value $elemMatch: observe type bracketing in the filter.",
        "[{$project: {a: {$literal: [1, 2, 3, 4]}}}, {$match: {a: {$elemMatch: {$gte: 2, $lte: "
        "3}}}}]");

    testABTTranslationAndOptimization(
        gctx,
        "$project then $match",
        "[{$project: {s: {$add: ['$a', '$b']}, c: 1}}, {$match: {$or: [{c: 2}, {s: {$gte: "
        "10}}]}}]");

    testABTTranslationAndOptimization(
        gctx, "$project with deeply nested path", "[{$project: {'a1.b.c':1, 'a.b.c.d.e':'str'}}]");

    testABTTranslationAndOptimization(
        gctx, "basic $group", "[{$group: {_id: '$a.b', s: {$sum: {$multiply: ['$b', '$c']}}}}]");

    testABTTranslationAndOptimization(
        gctx, "$group local global", "[{$group: {_id: '$a', c: {$sum: '$b'}}}]");

    testABTTranslationAndOptimization(gctx, "basic $unwind", "[{$unwind: {path: '$a.b.c'}}]");

    testABTTranslationAndOptimization(
        gctx,
        "complex $unwind",
        "[{$unwind: {path: '$a.b.c', includeArrayIndex: 'p1.pid', preserveNullAndEmptyArrays: "
        "true}}]");

    testABTTranslationAndOptimization(
        gctx,
        "$unwind with $group",
        "[{$unwind:{path: '$a.b', preserveNullAndEmptyArrays: true}}, {$group:{_id: '$a.b'}}]");

    testABTTranslationAndOptimization(gctx, "$match with index", "[{$match: {'a': 10}}]");

    testABTTranslationAndOptimization(
        gctx, "$match sort index", "[{$match: {'a': 10}}, {$sort: {'a': 1}}]");

    testABTTranslationAndOptimization(
        gctx, "$match with range", "[{$match: {'a': {$gt: 70, $lt: 90}}}]");

    testABTTranslationAndOptimization(gctx, "index on two keys", "[{$match: {'a': 2, 'b': 2}}]");

    testABTTranslationAndOptimization(
        gctx,
        "$group with complex _id",
        "[{$group: {_id: {'isin': '$isin', 'year': '$year'}, 'count': {$sum: 1}, 'open': {$first: "
        "'$$ROOT'}}}]");

    testABTTranslationAndOptimization(
        gctx, "$project with computed array", "[{$project: {a: ['$b', '$c']}}]");

    std::string scanDefA = "collA";
    std::string scanDefB = "collB";
    Metadata metadataUnion{{{scanDefA, {}}, {scanDefB, {}}}};
    testABTTranslationAndOptimization(gctx,
                                      "union",
                                      "[{$unionWith: 'collB'}, {$match: {_id: 1}}]",
                                      scanDefA,
                                      {},
                                      metadataUnion,
                                      {},
                                      false,
                                      {{NamespaceString("a." + scanDefB), {}}});

    testABTTranslationAndOptimization(gctx, "$match with $ne", "[{$match: {'a': {$ne: 2}}}]");
}
}  // namespace mongo::optimizer
