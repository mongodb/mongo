/**
 * End to end test for join optimization with additional filters.
 *
 * @tags: [
 *   requires_fcv_83,
 * ]
 */

import {runTest} from "jstests/libs/query/join_utils.js";

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

    assert.commandWorked(
        foreignColl1.insertMany([
            {_id: 0, a: 1, c: "foo", d: 1},
            {_id: 1, a: 1, c: "bar", d: 2},
            {_id: 2, a: 2, c: "baz", d: 1},
            {_id: 3, a: 2, c: "qux", d: 2},
        ]),
    );

    assert.commandWorked(
        foreignColl2.insertMany([
            {_id: 0, b: 1, e: "foo", f: 1},
            {_id: 1, b: 1, e: "bar", f: 2},
            {_id: 2, b: 2, e: "baz", f: 1},
        ]),
    );

    runTest({
        description: "Join optimization should not be used with local/foreignField syntax and additional filter",
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
        expectedUsedJoinOptimization: false,
    });

    // Same as above, without project.
    runTest({
        description:
            "Join optimization should not be used with local/foreignField syntax and additional filter without project",
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
        expectedUsedJoinOptimization: false,
    });

    runTest({
        description: "Join optimization should not be used with let/pipeline syntax and additional filter",
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
        expectedUsedJoinOptimization: false,
    });

    runTest({
        description: "Join optimization should not be used with field syntax and pipeline syntax and additional filter",
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
        expectedUsedJoinOptimization: false,
    });

    runTest({
        description: "Join optimization can be used on a prefix even when the suffix has absorbed an additional filter",
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
    });
} finally {
    assert.commandWorked(db.adminCommand({setParameter: 1, internalEnableJoinOptimization: false}));
}
