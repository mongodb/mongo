/**
 * End to end test for join optimization with a let variable referenced with a trailing path
 * suffix (e.g. '$$x.a' where 'let: {x: "$obj"}'). The optimizer must resolve such a reference to
 * the local path 'obj.a', not 'obj'; otherwise it compares a scalar foreign field against the
 * object 'obj' and produces no matches.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_sbe
 * ]
 */

import {runTestWithUnorderedComparison} from "jstests/libs/query/join_utils.js";

const conn = MongoRunner.runMongod({setParameter: {featureFlagPathArrayness: true}});
const db = conn.getDB(jsTestName());

const baseColl = db[jsTestName()];
const foreignColl = db[jsTestName() + "_foreign"];

baseColl.drop();
foreignColl.drop();

assert.commandWorked(
    baseColl.insertMany([
        {obj: {a: 1, d: 1}},
        {obj: {a: 1, d: 2}},
        {obj: {a: 2, d: 1}},
        {obj: {a: 2, d: 2}},
    ]),
);
// Indexes provide multikeyness/path arrayness info for the resolved local paths 'obj.a' and
// 'obj.d'. Without these the join optimizer would bail out and the bug would not be hit.
assert.commandWorked(baseColl.createIndex({dummy: 1, "obj.a": 1, "obj.d": 1}));

assert.commandWorked(
    foreignColl.insertMany([
        {a: 1, c: "foo", d: 1},
        {a: 1, c: "bar", d: 2},
        {a: 2, c: "baz", d: 1},
        {a: 2, c: "qux", d: 2},
    ]),
);
assert.commandWorked(foreignColl.createIndex({dummy: 1, a: 1, c: 1, d: 1}));

// Variable reference on the right operand: 'foreign.a == $$x.a' must resolve to
// 'foreign.a == base.obj.a'.
runTestWithUnorderedComparison({
    db,
    description: "Path suffix on let variable reference (variable on right)",
    coll: baseColl,
    pipeline: [
        {
            $lookup: {
                from: foreignColl.getName(),
                let: {x: "$obj"},
                pipeline: [
                    {$match: {$expr: {$and: [{$eq: ["$a", "$$x.a"]}, {$eq: ["$d", "$$x.d"]}]}}},
                ],
                as: "joined",
            },
        },
        {$unwind: "$joined"},
        {$project: {_id: 0, "joined._id": 0}},
    ],
    expectedResults: [
        {obj: {a: 1, d: 1}, joined: {a: 1, c: "foo", d: 1}},
        {obj: {a: 1, d: 2}, joined: {a: 1, c: "bar", d: 2}},
        {obj: {a: 2, d: 1}, joined: {a: 2, c: "baz", d: 1}},
        {obj: {a: 2, d: 2}, joined: {a: 2, c: "qux", d: 2}},
    ],
    expectedUsedJoinOptimization: true,
    expectedNumJoinStages: 1,
});

// Same shape, but with the variable reference on the left operand of $eq.
runTestWithUnorderedComparison({
    db,
    description: "Path suffix on let variable reference (variable on left)",
    coll: baseColl,
    pipeline: [
        {
            $lookup: {
                from: foreignColl.getName(),
                let: {x: "$obj"},
                pipeline: [
                    {$match: {$expr: {$and: [{$eq: ["$$x.a", "$a"]}, {$eq: ["$$x.d", "$d"]}]}}},
                ],
                as: "joined",
            },
        },
        {$unwind: "$joined"},
        {$project: {_id: 0, "joined._id": 0}},
    ],
    expectedResults: [
        {obj: {a: 1, d: 1}, joined: {a: 1, c: "foo", d: 1}},
        {obj: {a: 1, d: 2}, joined: {a: 1, c: "bar", d: 2}},
        {obj: {a: 2, d: 1}, joined: {a: 2, c: "baz", d: 1}},
        {obj: {a: 2, d: 2}, joined: {a: 2, c: "qux", d: 2}},
    ],
    expectedUsedJoinOptimization: true,
    expectedNumJoinStages: 1,
});

// Regression test: when both a localField join path ("key") and a let-bound path ("cor.key.foo")
// on the same local collection join to the same foreign field, implicit edge inference transitively
// infers local.key == local.cor.key.foo. This is a single-table predicate, not a join edge, and
// used to cause a tassert ("Self edges are not permitted") in addEdge().
{
    const localColl = db[jsTestName() + "_self_edge_local"];
    const otherColl = db[jsTestName() + "_self_edge_other"];
    localColl.drop();
    otherColl.drop();

    assert.commandWorked(
        localColl.insertMany([
            {key: 42, cor: {key: {foo: 42}}}, // Satisfies both join conditions, produces output.
            {key: 42, cor: {key: {foo: 99}}}, // base_other.key can't equal both simultaneously, no match.
        ]),
    );

    assert.commandWorked(otherColl.insertMany([{key: 42}]));

    // Indexes let path arrayness detect these paths as scalar so join optimization triggers.
    assert.commandWorked(localColl.createIndex({key: 1, "cor.key.foo": 1}));
    assert.commandWorked(otherColl.createIndex({key: 1}));

    runTestWithUnorderedComparison({
        db,
        description: "localField and let binding on same local collection do not produce self-edge",
        coll: localColl,
        pipeline: [
            {
                $lookup: {
                    from: otherColl.getName(),
                    localField: "key",
                    foreignField: "key",
                    let: {f: "$cor.key.foo"},
                    pipeline: [{$match: {$expr: {$eq: ["$key", "$$f"]}}}],
                    as: "lf",
                },
            },
            {$unwind: "$lf"},
            {$project: {_id: 0, "lf._id": 0}},
        ],
        expectedResults: [{key: 42, cor: {key: {foo: 42}}, lf: {key: 42}}],
        expectedUsedJoinOptimization: true,
    });
}

MongoRunner.stopMongod(conn);
