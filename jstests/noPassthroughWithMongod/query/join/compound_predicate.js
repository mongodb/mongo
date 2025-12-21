/**
 * End to end test for join optimization with compound join predicates.
 *
 * @tags: [
 *   requires_fcv_83,
 * ]
 */

import {runTest} from "jstests/libs/query/join_utils.js";

try {
    const baseColl = db[jsTestName()];
    const foreignColl1 = db[jsTestName() + "_a"];

    baseColl.drop();
    foreignColl1.drop();

    assert.commandWorked(
        baseColl.insertMany([
            {a: 1, b: 1, d: 1},
            {a: 1, b: 2, d: 2},
            {a: 2, b: 1, d: 1},
            {a: 2, b: 2, d: 2},
        ]),
    );

    assert.commandWorked(
        foreignColl1.insertMany([
            {a: 1, c: "foo", d: 1},
            {a: 1, c: "bar", d: 2},
            {a: 2, c: "baz", d: 1},
            {a: 2, c: "qux", d: 2},
        ]),
    );

    runTest({
        description: "Join optimization should be used with compound equality predicates",
        coll: baseColl,
        pipeline: [
            {
                $lookup: {
                    from: foreignColl1.getName(),
                    let: {a: "$a", d: "$d"},
                    pipeline: [{$match: {$expr: {$and: [{$eq: ["$a", "$$a"]}, {$eq: ["$d", "$$d"]}]}}}],
                    as: "foreignColl1",
                },
            },
            {$unwind: "$foreignColl1"},
            {$project: {_id: 0, "foreignColl1._id": 0}},
        ],
        expectedResults: [
            {a: 1, b: 1, d: 1, foreignColl1: {a: 1, c: "foo", d: 1}},
            {a: 1, b: 2, d: 2, foreignColl1: {a: 1, c: "bar", d: 2}},
            {a: 2, b: 1, d: 1, foreignColl1: {a: 2, c: "baz", d: 1}},
            {a: 2, b: 2, d: 2, foreignColl1: {a: 2, c: "qux", d: 2}},
        ],
        expectedUsedJoinOptimization: true,
    });

    runTest({
        description: "Join optimization should work with $$ROOT",
        coll: baseColl,
        pipeline: [
            {
                $lookup: {
                    from: foreignColl1.getName(),
                    let: {a: "$a", d: "$d"},
                    pipeline: [
                        {
                            $match: {
                                $expr: {
                                    $and: [
                                        {$eq: ["$a", "$$a"]},
                                        // Include a residual single table predicate referencing $$ROOT to verify that it works properly end to end.
                                        {$eq: ["$a", "$$ROOT.d"]},
                                    ],
                                },
                            },
                        },
                    ],
                    as: "foreignColl1",
                },
            },
            {$unwind: "$foreignColl1"},
            {$project: {_id: 0, "foreignColl1._id": 0}},
        ],
        expectedResults: [
            {a: 1, b: 1, d: 1, foreignColl1: {a: 1, c: "foo", d: 1}},
            {a: 1, b: 2, d: 2, foreignColl1: {a: 1, c: "foo", d: 1}},
            {a: 2, b: 1, d: 1, foreignColl1: {a: 2, c: "qux", d: 2}},
            {a: 2, b: 2, d: 2, foreignColl1: {a: 2, c: "qux", d: 2}},
        ],
        expectedUsedJoinOptimization: true,
    });
} finally {
    assert.commandWorked(db.adminCommand({setParameter: 1, internalEnableJoinOptimization: false}));
}
