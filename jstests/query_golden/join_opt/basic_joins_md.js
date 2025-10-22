/**
 * Run basic tests that validate we enter join ordering logic.
 *
 * @tags: [
 *   requires_fcv_83
 * ]
 */
import {section, subSection} from "jstests/libs/pretty_md.js";
import {outputAggregationPlanAndResults} from "jstests/libs/query/golden_test_utils.js";

const coll = db[jsTestName()];
coll.drop();
assert.commandWorked(
    coll.insertMany([
        {_id: 0, a: 1, b: "foo"},
        {_id: 1, a: 1, b: "bar"},
        {_id: 2, a: 2, b: "bar"},
    ]),
);

const foreignColl1 = db[jsTestName() + "_foreign1"];
foreignColl1.drop();
assert.commandWorked(
    foreignColl1.insertMany([
        {_id: 0, a: 1, c: "zoo", d: 1},
        {_id: 1, a: 2, c: "blah", d: 2},
        {_id: 2, a: 2, c: "x", d: 3},
    ]),
);

const foreignColl2 = db[jsTestName() + "_foreign2"];
foreignColl2.drop();
assert.commandWorked(
    foreignColl2.insertMany([
        {_id: 0, b: "bar", d: 2},
        {_id: 1, b: "bar", d: 6},
    ]),
);

function runBasicJoinTest(pipeline) {
    subSection("No join opt");
    assert.commandWorked(db.adminCommand({setParameter: 1, internalEnableJoinOptimization: false}));
    outputAggregationPlanAndResults(coll, pipeline, {}, true, false);

    subSection("With random order, seed 44");
    assert.commandWorked(db.adminCommand({setParameter: 1, internalEnableJoinOptimization: true}));
    assert.commandWorked(db.adminCommand({setParameter: 1, internalRandomJoinOrderSeed: 44}));
    outputAggregationPlanAndResults(coll, pipeline, {}, true, false);

    subSection("With random order, seed 420");
    assert.commandWorked(db.adminCommand({setParameter: 1, internalEnableJoinOptimization: true}));
    assert.commandWorked(db.adminCommand({setParameter: 1, internalRandomJoinOrderSeed: 420}));
    outputAggregationPlanAndResults(coll, pipeline, {}, true, false);

    // Reset flag.
    assert.commandWorked(db.adminCommand({setParameter: 1, internalEnableJoinOptimization: false}));
}

section("Basic example with two joins");
runBasicJoinTest([
    {$lookup: {from: foreignColl1.getName(), as: "x", localField: "a", foreignField: "a"}},
    {$unwind: "$x"},
    {$lookup: {from: foreignColl2.getName(), as: "y", localField: "b", foreignField: "b"}},
    {$unwind: "$y"},
]);

section("Basic example with two joins and suffix");
runBasicJoinTest([
    {$lookup: {from: foreignColl1.getName(), as: "x", localField: "a", foreignField: "a"}},
    {$unwind: "$x"},
    {$lookup: {from: foreignColl2.getName(), as: "y", localField: "b", foreignField: "b"}},
    {$unwind: "$y"},
    {$sortByCount: "$y.b"},
]);

// TODO SERVER-111910: Enable $lookup stages with sub-pipelines for join-opt, and add access-path selection tests.
// runBasicJoinTest([
//     {
//         $lookup: {
//             from: foreignColl1.getName(),
//             as: "x",
//             localField: "a",
//             foreignField: "a",
//             pipeline: [{$match: {d: {$lt: 3}}}, {$project: {_id: 0, a: 1}}],
//         },
//     },
//     {$unwind: "$x"},
//     {
//         $lookup: {
//             from: foreignColl2.getName(),
//             as: "y",
//             localField: "b",
//             foreignField: "b",
//             pipeline: [{$project: {_id: 0, b: 1}}],
//         },
//     },
//     {$unwind: "$y"},
//     {$sortByCount: "$x.a"},
// ]);

const foreignColl3 = db[jsTestName() + "_foreign3"];
foreignColl3.drop();
assert.commandWorked(
    foreignColl3.insertMany([
        {_id: 0, a: 1, c: "zoo", d: 1},
        {_id: 1, a: 2, c: "blah", d: 2},
        {_id: 2, a: 2, c: "x", d: 3},
    ]),
);

section("Basic example with referencing field from previous lookup");
runBasicJoinTest([
    {$lookup: {from: foreignColl1.getName(), as: "x", localField: "a", foreignField: "a"}},
    {$unwind: "$x"},
    {$lookup: {from: foreignColl3.getName(), as: "z", localField: "x.c", foreignField: "c"}},
    {$unwind: "$z"},
]);

section("Basic example with 3 joins & subsequent join referencing fields from previous lookups");
runBasicJoinTest([
    {$lookup: {from: foreignColl1.getName(), as: "x", localField: "a", foreignField: "a"}},
    {$unwind: "$x"},
    {$lookup: {from: foreignColl2.getName(), as: "y", localField: "b", foreignField: "b"}},
    {$unwind: "$y"},
    {$lookup: {from: foreignColl3.getName(), as: "z", localField: "x.c", foreignField: "c"}},
    {$unwind: "$z"},
]);

section("Basic example with 3 joins & subsequent join referencing nested paths");
runBasicJoinTest([
    {$lookup: {from: foreignColl1.getName(), as: "x", localField: "a", foreignField: "a"}},
    {$unwind: "$x"},
    {$lookup: {from: foreignColl3.getName(), as: "x.y", localField: "x.c", foreignField: "c"}},
    {$unwind: "$x.y"},
    {$lookup: {from: foreignColl2.getName(), as: "x.y.z", localField: "x.y.d", foreignField: "d"}},
    {$unwind: "$x.y.z"},
]);
