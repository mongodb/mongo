/**
 * End to end test for join optimization with additional filters.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_sbe,
 * ]
 */

import {runTestWithUnorderedComparison} from "jstests/libs/query/join_utils.js";

try {
    const baseColl = db[jsTestName()];
    const foreignColl1 = db[jsTestName() + "_a"];
    const foreignColl2 = db[jsTestName() + "_b"];

    baseColl.drop();
    foreignColl1.drop();

    assert.commandWorked(
        baseColl.insertMany([
            {_id: 0, a: 1, b: 1, d: 1},
            {_id: 1, a: 1, b: 2, d: 2},
            {_id: 2, a: 2, b: 1, d: 1},
            {_id: 3, a: 2, b: 2, d: 2},
        ]),
    );
    // Add index for multikeyness info for path arrayness.
    assert.commandWorked(baseColl.createIndex({dummy: 1, a: 1, b: 1, d: 1}));

    assert.commandWorked(
        foreignColl1.insertMany([
            {_id: 0, a: 1, c: "foo", d: 1},
            {_id: 1, a: 1, c: "bar", d: 2},
            {_id: 2, a: 2, c: "baz", d: 1},
            {_id: 3, a: 2, c: "qux", d: 2},
        ]),
    );
    // Add index for multikeyness info for path arrayness.
    assert.commandWorked(foreignColl1.createIndex({dummy: 1, a: 1, c: 1, d: 1}));

    assert.commandWorked(
        foreignColl2.insertMany([
            {_id: 0, b: 1, e: "foo", f: 1},
            {_id: 1, b: 1, e: "bar", f: 2},
            {_id: 2, b: 2, e: "baz", f: 1},
        ]),
    );
    // Add index for multikeyness info for path arrayness.
    assert.commandWorked(foreignColl2.createIndex({dummy: 1, b: 1, e: 1, f: 1}));

    runTestWithUnorderedComparison({
        db,
        description: "Join optimization should be used with local/foreignField syntax and additional filter",
        coll: baseColl,
        pipeline: [
            {
                $lookup: {
                    from: foreignColl1.getName(),
                    localField: "a",
                    foreignField: "a",
                    as: "foreignColl1",
                },
            },
            {$unwind: "$foreignColl1"},
            {$match: {"foreignColl1.c": {$eq: "bar"}}},
            {$project: {_id: 0, "foreignColl1._id": 0}},
        ],
        expectedResults: [
            {a: 1, b: 1, d: 1, foreignColl1: {a: 1, c: "bar", d: 2}},
            {a: 1, b: 2, d: 2, foreignColl1: {a: 1, c: "bar", d: 2}},
        ],
        expectedUsedJoinOptimization: true,
    });

    // Same as above, without project.
    runTestWithUnorderedComparison({
        db,
        description:
            "Join optimization should be used with local/foreignField syntax and additional filter without project",
        coll: baseColl,
        pipeline: [
            {
                $lookup: {
                    from: foreignColl1.getName(),
                    localField: "a",
                    foreignField: "a",
                    as: "foreignColl1",
                },
            },
            {$unwind: "$foreignColl1"},
            {$match: {"foreignColl1.c": {$eq: "bar"}}},
        ],
        expectedResults: [
            {_id: 0, a: 1, b: 1, d: 1, foreignColl1: {_id: 1, a: 1, c: "bar", d: 2}},
            {_id: 1, a: 1, b: 2, d: 2, foreignColl1: {_id: 1, a: 1, c: "bar", d: 2}},
        ],
        expectedUsedJoinOptimization: true,
    });

    runTestWithUnorderedComparison({
        db,
        description:
            "Join optimization should be used with two consecutive additional filters on local/foreignField syntax",
        coll: baseColl,
        pipeline: [
            {
                $lookup: {
                    from: foreignColl1.getName(),
                    localField: "a",
                    foreignField: "a",
                    as: "foreignColl1",
                },
            },
            {$unwind: "$foreignColl1"},
            {$match: {"foreignColl1.c": {$eq: "bar"}}},
            {$match: {"foreignColl1.d": {$eq: 2}}},
        ],
        expectedResults: [
            {_id: 0, a: 1, b: 1, d: 1, foreignColl1: {_id: 1, a: 1, c: "bar", d: 2}},
            {_id: 1, a: 1, b: 2, d: 2, foreignColl1: {_id: 1, a: 1, c: "bar", d: 2}},
        ],
        expectedUsedJoinOptimization: true,
    });

    runTestWithUnorderedComparison({
        db,
        description: "Join optimization should be used with let/pipeline syntax and additional filter",
        coll: baseColl,
        pipeline: [
            {
                $lookup: {
                    from: foreignColl1.getName(),
                    let: {a: "$a"},
                    pipeline: [{$match: {$expr: {$eq: ["$a", "$$a"]}}}],
                    as: "foreignColl1",
                },
            },
            {$unwind: "$foreignColl1"},
            {$match: {"foreignColl1.c": {$eq: "bar"}}},
            {$project: {_id: 0, "foreignColl1._id": 0}},
        ],
        expectedResults: [
            {a: 1, b: 1, d: 1, foreignColl1: {a: 1, c: "bar", d: 2}},
            {a: 1, b: 2, d: 2, foreignColl1: {a: 1, c: "bar", d: 2}},
        ],
        expectedUsedJoinOptimization: true,
    });

    runTestWithUnorderedComparison({
        db,
        description: "Join optimization should be used with pipeline: [] syntax and additional filter",
        coll: baseColl,
        pipeline: [
            {
                $lookup: {
                    from: foreignColl1.getName(),
                    localField: "a",
                    foreignField: "a",
                    pipeline: [],
                    as: "foreignColl1",
                },
            },
            {$unwind: "$foreignColl1"},
            {$match: {"foreignColl1.c": {$eq: "bar"}}},
        ],
        expectedResults: [
            {_id: 0, a: 1, b: 1, d: 1, foreignColl1: {_id: 1, a: 1, c: "bar", d: 2}},
            {_id: 1, a: 1, b: 2, d: 2, foreignColl1: {_id: 1, a: 1, c: "bar", d: 2}},
        ],
        expectedUsedJoinOptimization: true,
    });

    runTestWithUnorderedComparison({
        db,
        description: "Join optimization should be used with field syntax and pipeline syntax and additional filter",
        coll: baseColl,
        pipeline: [
            {
                $lookup: {
                    from: foreignColl1.getName(),
                    localField: "a",
                    foreignField: "a",
                    let: {d: "$d"},
                    pipeline: [{$match: {$expr: {$eq: ["$d", "$$d"]}}}],
                    as: "foreignColl1",
                },
            },
            {$unwind: "$foreignColl1"},
            {$match: {"foreignColl1.c": {$eq: "bar"}}},
            {$project: {_id: 0, "foreignColl1._id": 0}},
        ],
        expectedResults: [{a: 1, b: 2, d: 2, foreignColl1: {a: 1, c: "bar", d: 2}}],
        expectedUsedJoinOptimization: true,
    });

    runTestWithUnorderedComparison({
        db,
        description:
            "Join optimization should be used with sub-pipeline containing both $expr join + single-table predicate plus absorbed filter",
        coll: baseColl,
        pipeline: [
            {
                $lookup: {
                    from: foreignColl1.getName(),
                    let: {a: "$a"},
                    pipeline: [{$match: {$and: [{$expr: {$eq: ["$a", "$$a"]}}, {d: 1}]}}],
                    as: "foreignColl1",
                },
            },
            {$unwind: "$foreignColl1"},
            {$match: {"foreignColl1.c": {$eq: "foo"}}},
        ],
        expectedResults: [
            {_id: 0, a: 1, b: 1, d: 1, foreignColl1: {_id: 0, a: 1, c: "foo", d: 1}},
            {_id: 1, a: 1, b: 2, d: 2, foreignColl1: {_id: 0, a: 1, c: "foo", d: 1}},
        ],
        expectedUsedJoinOptimization: true,
    });

    runTestWithUnorderedComparison({
        db,
        description: "Join optimization should be used with multi-predicate sub-pipeline $match and absorbed filter",
        coll: baseColl,
        pipeline: [
            {
                $lookup: {
                    from: foreignColl1.getName(),
                    localField: "a",
                    foreignField: "a",
                    pipeline: [{$match: {a: 1, d: 1}}],
                    as: "foreignColl1",
                },
            },
            {$unwind: "$foreignColl1"},
            {$match: {"foreignColl1.c": {$eq: "foo"}}},
        ],
        expectedResults: [
            {_id: 0, a: 1, b: 1, d: 1, foreignColl1: {_id: 0, a: 1, c: "foo", d: 1}},
            {_id: 1, a: 1, b: 2, d: 2, foreignColl1: {_id: 0, a: 1, c: "foo", d: 1}},
        ],
        expectedUsedJoinOptimization: true,
    });

    runTestWithUnorderedComparison({
        db,
        description:
            "Join optimization should be used with pipeline $lookup and two consecutive trailing $match stages",
        coll: baseColl,
        pipeline: [
            {
                $lookup: {
                    from: foreignColl1.getName(),
                    localField: "a",
                    foreignField: "a",
                    pipeline: [{$match: {d: 1}}],
                    as: "foreignColl1",
                },
            },
            {$unwind: "$foreignColl1"},
            {$match: {"foreignColl1.c": {$eq: "foo"}}},
            {$match: {"foreignColl1.a": {$eq: 1}}},
        ],
        expectedResults: [
            {_id: 0, a: 1, b: 1, d: 1, foreignColl1: {_id: 0, a: 1, c: "foo", d: 1}},
            {_id: 1, a: 1, b: 2, d: 2, foreignColl1: {_id: 0, a: 1, c: "foo", d: 1}},
        ],
        expectedUsedJoinOptimization: true,
    });

    runTestWithUnorderedComparison({
        db,
        description:
            "Non-equijoin $expr in sub-pipeline with absorbed filter should fall back to non-optimized execution",
        coll: baseColl,
        pipeline: [
            {
                $lookup: {
                    from: foreignColl1.getName(),
                    let: {a: "$a"},
                    pipeline: [{$match: {$expr: {$gt: ["$a", "$$a"]}}}],
                    as: "foreignColl1",
                },
            },
            {$unwind: "$foreignColl1"},
            {$match: {"foreignColl1.c": {$eq: "baz"}}},
        ],
        expectedResults: [
            {_id: 0, a: 1, b: 1, d: 1, foreignColl1: {_id: 2, a: 2, c: "baz", d: 1}},
            {_id: 1, a: 1, b: 2, d: 2, foreignColl1: {_id: 2, a: 2, c: "baz", d: 1}},
        ],
        expectedUsedJoinOptimization: false,
    });

    runTestWithUnorderedComparison({
        db,
        description:
            "Join optimization should absorb an additional filter on a chained $lookup whose join key references a previous $lookup's as-field",
        coll: baseColl,
        pipeline: [
            {
                $lookup: {
                    from: foreignColl1.getName(),
                    localField: "a",
                    foreignField: "a",
                    as: "foreignColl1",
                },
            },
            {$unwind: "$foreignColl1"},
            {
                $lookup: {
                    from: foreignColl2.getName(),
                    localField: "foreignColl1.c",
                    foreignField: "e",
                    as: "foreignColl2",
                },
            },
            {$unwind: "$foreignColl2"},
            {$match: {"foreignColl2.f": {$eq: 2}}},
            {$project: {_id: 0, "foreignColl1._id": 0, "foreignColl2._id": 0}},
        ],
        expectedResults: [
            {a: 1, b: 1, d: 1, foreignColl1: {a: 1, c: "bar", d: 2}, foreignColl2: {b: 1, e: "bar", f: 2}},
            {a: 1, b: 2, d: 2, foreignColl1: {a: 1, c: "bar", d: 2}, foreignColl2: {b: 1, e: "bar", f: 2}},
        ],
        expectedUsedJoinOptimization: true,
        expectedNumJoinStages: 2,
    });

    runTestWithUnorderedComparison({
        db,
        description: "Join optimization can handle filter which leads to EOF QSN",
        coll: baseColl,
        pipeline: [
            {
                $lookup: {
                    from: foreignColl1.getName(),
                    localField: "a",
                    foreignField: "a",
                    as: "foreignColl1",
                },
            },
            {$unwind: "$foreignColl1"},
            {$match: {$alwaysFalse: 1}},
        ],
        expectedResults: [],
        expectedUsedJoinOptimization: true,
    });

    runTestWithUnorderedComparison({
        db,
        description:
            "Join optimization should handle two consecutive $matches on the same field of the joined collection",
        coll: baseColl,
        pipeline: [
            {
                $lookup: {
                    from: foreignColl1.getName(),
                    localField: "a",
                    foreignField: "a",
                    as: "foreignColl1",
                },
            },
            {$unwind: "$foreignColl1"},
            {$match: {"foreignColl1.d": {$gte: 2}}},
            {$match: {"foreignColl1.d": {$lte: 2}}},
        ],
        expectedResults: [
            {_id: 0, a: 1, b: 1, d: 1, foreignColl1: {_id: 1, a: 1, c: "bar", d: 2}},
            {_id: 1, a: 1, b: 2, d: 2, foreignColl1: {_id: 1, a: 1, c: "bar", d: 2}},
            {_id: 2, a: 2, b: 1, d: 1, foreignColl1: {_id: 3, a: 2, c: "qux", d: 2}},
            {_id: 3, a: 2, b: 2, d: 2, foreignColl1: {_id: 3, a: 2, c: "qux", d: 2}},
        ],
        expectedUsedJoinOptimization: true,
    });

    runTestWithUnorderedComparison({
        db,
        description:
            "Join optimization should route two consecutive $matches each targeting a different joined collection",
        coll: baseColl,
        pipeline: [
            {
                $lookup: {
                    from: foreignColl1.getName(),
                    localField: "a",
                    foreignField: "a",
                    as: "foreignColl1",
                },
            },
            {$unwind: "$foreignColl1"},
            {
                $lookup: {
                    from: foreignColl2.getName(),
                    localField: "b",
                    foreignField: "b",
                    as: "foreignColl2",
                },
            },
            {$unwind: "$foreignColl2"},
            {$match: {"foreignColl1.c": {$eq: "bar"}}},
            {$match: {"foreignColl2.e": {$eq: "foo"}}},
        ],
        expectedResults: [
            {
                _id: 0,
                a: 1,
                b: 1,
                d: 1,
                foreignColl1: {_id: 1, a: 1, c: "bar", d: 2},
                foreignColl2: {_id: 0, b: 1, e: "foo", f: 1},
            },
        ],
        expectedUsedJoinOptimization: true,
        expectedNumJoinStages: 2,
    });

    runTestWithUnorderedComparison({
        db,
        description: "Join optimization should absorb a $match placed between two $lookup/$unwind pairs",
        coll: baseColl,
        pipeline: [
            {
                $lookup: {
                    from: foreignColl1.getName(),
                    localField: "a",
                    foreignField: "a",
                    as: "foreignColl1",
                },
            },
            {$unwind: "$foreignColl1"},
            {$match: {"foreignColl1.c": {$eq: "bar"}}},
            {
                $lookup: {
                    from: foreignColl2.getName(),
                    localField: "b",
                    foreignField: "b",
                    as: "foreignColl2",
                },
            },
            {$unwind: "$foreignColl2"},
        ],
        expectedResults: [
            {
                _id: 0,
                a: 1,
                b: 1,
                d: 1,
                foreignColl1: {_id: 1, a: 1, c: "bar", d: 2},
                foreignColl2: {_id: 0, b: 1, e: "foo", f: 1},
            },
            {
                _id: 0,
                a: 1,
                b: 1,
                d: 1,
                foreignColl1: {_id: 1, a: 1, c: "bar", d: 2},
                foreignColl2: {_id: 1, b: 1, e: "bar", f: 2},
            },
            {
                _id: 1,
                a: 1,
                b: 2,
                d: 2,
                foreignColl1: {_id: 1, a: 1, c: "bar", d: 2},
                foreignColl2: {_id: 2, b: 2, e: "baz", f: 1},
            },
        ],
        expectedUsedJoinOptimization: true,
        expectedNumJoinStages: 2,
    });

    runTestWithUnorderedComparison({
        db,
        description: "Join optimization should split a $match with predicates on both base and joined collection",
        coll: baseColl,
        pipeline: [
            {
                $lookup: {
                    from: foreignColl1.getName(),
                    localField: "a",
                    foreignField: "a",
                    as: "foreignColl1",
                },
            },
            {$unwind: "$foreignColl1"},
            {$match: {"foreignColl1.c": {$eq: "bar"}, "b": 1}},
        ],
        expectedResults: [{_id: 0, a: 1, b: 1, d: 1, foreignColl1: {_id: 1, a: 1, c: "bar", d: 2}}],
        expectedUsedJoinOptimization: true,
    });

    runTestWithUnorderedComparison({
        db,
        description:
            "Join optimization should route a single $match with predicates on two different joined collections",
        coll: baseColl,
        pipeline: [
            {
                $lookup: {
                    from: foreignColl1.getName(),
                    localField: "a",
                    foreignField: "a",
                    as: "foreignColl1",
                },
            },
            {$unwind: "$foreignColl1"},
            {
                $lookup: {
                    from: foreignColl2.getName(),
                    localField: "b",
                    foreignField: "b",
                    as: "foreignColl2",
                },
            },
            {$unwind: "$foreignColl2"},
            {$match: {"foreignColl1.c": {$eq: "bar"}, "foreignColl2.e": {$eq: "foo"}}},
        ],
        expectedResults: [
            {
                _id: 0,
                a: 1,
                b: 1,
                d: 1,
                foreignColl1: {_id: 1, a: 1, c: "bar", d: 2},
                foreignColl2: {_id: 0, b: 1, e: "foo", f: 1},
            },
        ],
        expectedUsedJoinOptimization: true,
        expectedNumJoinStages: 2,
    });

    runTestWithUnorderedComparison({
        db,
        description: "Join optimization can handle filter which leads to EOF QSN in subpipeline",
        coll: baseColl,
        pipeline: [
            {
                $lookup: {
                    from: foreignColl1.getName(),
                    localField: "a",
                    foreignField: "a",
                    pipeline: [{$match: {$alwaysFalse: 1}}],
                    as: "foreignColl1",
                },
            },
            {$unwind: "$foreignColl1"},
        ],
        expectedResults: [],
        expectedUsedJoinOptimization: true,
    });
} finally {
    assert.commandWorked(db.adminCommand({setParameter: 1, internalEnableJoinOptimization: false}));
}
