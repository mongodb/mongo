/**
 * Regression test for SERVER-126487.
 *
 * Demonstrates incorrect results when join optimization reorders $lookup stages such that a later
 * $lookup writes to a top-level path ('a') that is the prefix of an earlier $lookup's 'localField'
 * ('a.x'). After reordering, the localField predicate must continue to read the original base
 * document's 'a.x', not the value embedded by the reordered-ahead $lookup's 'as: "a"'.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_sbe,
 *   featureFlagPathArrayness,
 * ]
 */
import {runTestWithUnorderedComparison} from "jstests/libs/query/join_utils.js";

const baseColl = db[jsTestName() + "_base"];
const matchColl = db[jsTestName() + "_match"];
const shadowColl = db[jsTestName() + "_shadow"];

baseColl.drop();
matchColl.drop();
shadowColl.drop();

// base.a.x == 1; the first $lookup uses localField:"a.x" against matchColl.k.
assert.commandWorked(baseColl.insertOne({_id: 0, a: {x: 1}, key: 1}));
assert.commandWorked(matchColl.insertOne({_id: 0, k: 1}));
// shadowColl is joined via key==key; its document has 'x: -1', which would shadow base.a.x if the
// reordered plan misreads "a.x" after the second $lookup overwrites top-level "a".
assert.commandWorked(shadowColl.insertOne({_id: 0, key: 1, x: -1}));

assert.commandWorked(baseColl.createIndexes([{key: 1}, {"a.x": 1}]));
assert.commandWorked(matchColl.createIndex({k: 1}));
assert.commandWorked(shadowColl.createIndex({key: 1}));

const pipeline = [
    {$lookup: {from: matchColl.getName(), localField: "a.x", foreignField: "k", as: "matched"}},
    {$unwind: "$matched"},
    {$lookup: {from: shadowColl.getName(), localField: "key", foreignField: "key", as: "a"}},
    {$unwind: "$a"},
];

// Expected reference result with join optimization disabled: one document, since base.a.x == 1
// matches matchColl.k == 1, and base.key == 1 matches shadowColl.key == 1.
const expectedResults = baseColl
    .aggregate(pipeline, {$_internalEnableJoinOptimization: false})
    .toArray();
assert.eq(
    expectedResults.length,
    1,
    "Reference (join opt disabled): expected 1 document, got " + tojson(expectedResults),
);

// Run with join optimization enabled in random-reorder mode using a fixed seed so the reordered
// plan is reproducible. The bug surfaces when $lookup{as:"a"} runs before $lookup{localField:"a.x"}.
runTestWithUnorderedComparison({
    db: db,
    description: "SERVER-126487: localField 'a.x' must not be shadowed by reordered $lookup as:'a'",
    coll: baseColl,
    pipeline: pipeline,
    expectedResults: expectedResults,
    expectedUsedJoinOptimization: true,
    additionalJoinParams: {
        internalJoinReorderMode: "random",
        internalRandomJoinOrderSeed: 0,
    },
});
