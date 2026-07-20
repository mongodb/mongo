/**
 * Run a variety of tests to exercise projection & rename logic.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_sbe,
 * ]
 */
import {code, section, subSection} from "jstests/libs/query/pretty_md.js";
import {runJoinTestAndCompare} from "jstests/query_golden/libs/join_opt.js";
import {normalizeArray} from "jstests/libs/query_optimization/golden_test.js";
import {joinTestWrapper, joinOptUsed} from "jstests/libs/query/join_utils.js";

const a = db[jsTestName() + "_a"];
a.drop();
assert.commandWorked(
    a.insertMany([
        {_id: 1, a: 1, obj: {subobj: {field: "foo"}}},
        {_id: 2, a: 2, obj: {subobj: {field: "bar"}}},
        {_id: 3, a: 3, obj: {subobj: {field: "foo"}}},
    ]),
);
assert.commandWorked(a.createIndex({a: 1, "obj.subobj.field": 1}));

const b = db[jsTestName() + "_b"];
b.drop();
assert.commandWorked(
    b.insertMany([
        {_id: 1, b: 1, obj: {foo: "foo"}},
        {_id: 2, b: -1, obj: {foo: "bar"}},
    ]),
);
assert.commandWorked(b.createIndex({b: 1, "obj.foo": 1}));

function runJoinTest(test, baseColl, pipeline, assertResultsEqual = true) {
    section(test);
    assert.commandWorked(db.adminCommand({setParameter: 1, internalEnableJoinOptimization: false}));

    subSection("Pipeline");
    code(tojson(pipeline));

    const noJoinOptResults = baseColl.aggregate(pipeline).toArray();
    subSection("Results");
    code(normalizeArray(noJoinOptResults));

    const noJoinExplain = baseColl.explain().aggregate(pipeline);
    assert(
        !joinOptUsed(noJoinExplain),
        "Join optimizer was used unexpectedly: " + tojson(noJoinExplain),
    );

    runJoinTestAndCompare(
        `${test} + Join Optimization`,
        baseColl,
        pipeline,
        {internalEnableJoinOptimization: true},
        noJoinOptResults,
        assertResultsEqual,
    );
}

joinTestWrapper(db, () => {
    runJoinTest("2-Nodes, Simple rename join preds (local/foreign)", a, [
        {$project: {"x": "$a"}},
        {
            $lookup: {
                from: b.getName(),
                localField: "x",
                foreignField: "b",
                as: "j1",
                pipeline: [{$project: {"y": "$b"}}],
            },
        },
        {$unwind: "$j1"},
    ]);

    runJoinTest("2-Nodes, Simple rename join preds (subpipeline $match)", a, [
        {$project: {"x": "$a"}},
        {
            $lookup: {
                from: b.getName(),
                let: {v: "$x"},
                as: "j1",
                pipeline: [{$match: {$expr: {$eq: ["$b", "$$v"]}}}, {$project: {"y": "$b"}}],
            },
        },
        {$unwind: "$j1"},
    ]);

    runJoinTest("2-Nodes, Simple rename join preds (trailing $match)", a, [
        {$project: {"x": "$a"}},
        {$lookup: {from: b.getName(), as: "j1", pipeline: [{$project: {"y": "$b"}}]}},
        {$unwind: "$j1"},
        {$match: {$expr: {$eq: ["$j1.y", "$x"]}}},
    ]);

    runJoinTest("2-Nodes, Complex rename join preds (trailing $match)", a, [
        {$project: {"x": "$obj.subobj.field"}},
        {$lookup: {from: b.getName(), as: "j1", pipeline: [{$project: {"y": "$obj.foo"}}]}},
        {$unwind: "$j1"},
        {$match: {$expr: {$eq: ["$j1.y", "$x"]}}},
    ]);

    runJoinTest("4-Nodes, Rename all join preds", a, [
        {$project: {"x.y": "$a", "z": "$obj.subobj.field"}},
        {
            $lookup: {
                from: b.getName(),
                as: "j1",
                pipeline: [{$project: {"m.n": "$obj.foo", "m.o": "$b"}}],
            },
        },
        {$unwind: "$j1"},
        {$lookup: {from: a.getName(), as: "j2", pipeline: []}},
        {$unwind: "$j2"},
        {
            $lookup: {
                from: b.getName(),
                as: "j3",
                pipeline: [{$project: {"obj.obj.obj.foo": "$obj.foo", "obj.obj.b": "$b"}}],
            },
        },
        {$unwind: "$j3"},
        {
            $match: {
                $expr: {
                    $and: [
                        {$eq: ["$x.y", "$j2.a"]},
                        {$eq: ["$j3.obj.obj.obj.foo", "$j2.obj.subobj.field"]},
                        {$eq: ["$z", "$j1.m.n"]},
                        {$eq: ["$j1.m.o", "$j3.obj.obj.b"]},
                    ],
                },
            },
        },
    ]);

    runJoinTest("2-Nodes, Exclusion projection (local/foreign)", a, [
        {$project: {"obj": 0}},
        {
            $lookup: {
                from: b.getName(),
                localField: "a",
                foreignField: "b",
                as: "j1",
                pipeline: [{$project: {"obj": 0}}],
            },
        },
        {$unwind: "$j1"},
    ]);

    runJoinTest("2-Nodes, Exclusion projection (trailing $match)", a, [
        {$project: {"obj": 0}},
        {$lookup: {from: b.getName(), as: "j1", pipeline: [{$project: {"obj.foo": 0}}]}},
        {$unwind: "$j1"},
        {$match: {$expr: {$eq: ["$j1.b", "$a"]}}},
    ]);

    // TODO SERVER-131452: this produces incorrect results.
    runJoinTest(
        "2-Nodes, Exclusion projection on subobject (trailing $match)",
        a,
        [
            {$project: {"obj.subobj": 0}},
            {$lookup: {from: b.getName(), as: "j1", pipeline: [{$project: {"_id": 0}}]}},
            {$unwind: "$j1"},
            {$match: {$expr: {$eq: ["$j1.b", "$a"]}}},
        ],
        false /* assertResultsEqual */,
    );

    // TODO SERVER-131449: Ensure the last $lookup is eligible after fix to deps graph.
    runJoinTest("4-Nodes, Rename all join preds, subpipeline edges", a, [
        {$project: {"x.y": "$a", "z": "$obj.subobj.field"}},
        {
            $lookup: {
                from: b.getName(),
                localField: "z",
                foreignField: "obj.foo",
                as: "j1",
                pipeline: [{$project: {"m.n": "$obj.foo", "m.o": "$b"}}],
            },
        },
        {$unwind: "$j1"},
        {
            $lookup: {
                from: a.getName(),
                as: "j2",
                let: {o1: "$x.y"},
                pipeline: [
                    {$match: {$expr: {$eq: ["$a", "$$o1"]}}},
                    {$project: {"rename.some.field": "$obj.subobj.field", "a": 1}},
                ],
            },
        },
        {$unwind: "$j2"},
        {
            $lookup: {
                from: b.getName(),
                as: "j3",
                localField: "j1.m.o",
                foreignField: "b",
                let: {o2: "$j2.rename.some.field"},
                pipeline: [
                    {$match: {$expr: {$eq: ["$obj.foo", "$$o2"]}}},
                    {$project: {"obj.obj.obj.foo": "$obj.foo", "obj.obj.b": "$b"}},
                ],
            },
        },
        {$unwind: "$j3"},
    ]);
});
