/**
 * Verifies that we correcly process overrding local fields by foreign documents.
 * @tags: [
 *   requires_fcv_90,
 *   requires_sbe
 * ]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";

const docs = [
    {_id: "first", a: 1, b: 1},
    {_id: "second", a: 1, b: 2},
];

const config = {
    setParameter: {
        internalEnableJoinOptimization: true,
        featureFlagPathArrayness: true,
    },
};

const conn = MongoRunner.runMongod(config);

const db = conn.getDB(jsTestName());

db.coll.drop();
assert.commandWorked(db.coll.insertMany(docs));
// Add index for multikeyness info for path arrayness.
assert.commandWorked(db.coll.createIndex({dummy: 1, a: 1, b: 1}));

const pipeline = [
    {$lookup: {from: "coll", localField: "_id", foreignField: "_id", as: "_id"}},
    {$unwind: "$_id"},
    {$lookup: {from: "coll", localField: "a", foreignField: "b", as: "a"}},
    {$unwind: "$a"},
    {$lookup: {from: "coll", localField: "b", foreignField: "b", as: "b"}},
    {$unwind: "$b"},
];

const actual = db.coll.aggregate(pipeline).toArray();

// Regression test: a $lookup whose "as" field is a prefix of a local field referenced through a
// `let` variable in the sub-pipeline's $expr (here `as: "a"` shadows the local `a.x`). The let
// variable is evaluated against the input document before the foreign result overwrites "a", so
// `a.x` must resolve to the base collection, not to the just-added foreign node. Otherwise both
// sides of the join predicate resolve to the same node and the join optimizer hits an illegal
// self-edge.
db.prefix.drop();
db.prefixForeign.drop();
assert.commandWorked(db.prefix.insert({_id: 0, a: {x: 1}}));
assert.commandWorked(db.prefixForeign.insert({_id: "f", z: 1}));
assert.commandWorked(db.prefix.createIndex({"a.x": 1}));
assert.commandWorked(db.prefixForeign.createIndex({z: 1}));

const prefixPipeline = [
    {
        $lookup: {
            from: "prefixForeign",
            as: "a",
            let: {l: "$a.x"},
            pipeline: [{$match: {$expr: {$eq: ["$z", "$$l"]}}}],
        },
    },
    {$unwind: "$a"},
];
const prefixActual = db.prefix.aggregate(prefixPipeline).toArray();

// Regression test: the second $lookup's localField "a.x" refers to field "x" inside the joined
// document from A (because the first $lookup already replaced "a" with the A result), not to the
// original base document's "a.x". The join optimizer must attribute "a.x" to the A node after the
// first join is established. We deliberately set the original base a.x (99) to a different value
// than A.x (10) so that using the wrong node would yield no match in B and an empty result.
db.chainL.drop();
db.chainA.drop();
db.chainB.drop();
assert.commandWorked(db.chainL.insert({_id: 0, a: {w: 1, x: 99}}));
assert.commandWorked(db.chainA.insert({_id: "a0", w: 1, x: 10}));
assert.commandWorked(db.chainB.insert({_id: "b0", x: 10}));
assert.commandWorked(db.chainL.createIndex({"a.w": 1}));
assert.commandWorked(db.chainA.createIndex({w: 1, x: 1}));
assert.commandWorked(db.chainB.createIndex({x: 1}));

const chainPipeline = [
    {$lookup: {from: "chainA", localField: "a.w", foreignField: "w", as: "a"}},
    {$unwind: "$a"},
    {$lookup: {from: "chainB", localField: "a.x", foreignField: "x", as: "b"}},
    {$unwind: "$b"},
];
const chainActual = db.chainL.aggregate(chainPipeline).toArray();

MongoRunner.stopMongod(conn);

assertArrayEq({actual: prefixActual, expected: [{_id: 0, a: {_id: "f", z: 1}}]});
assertArrayEq({
    actual: chainActual,
    expected: [{_id: 0, a: {_id: "a0", w: 1, x: 10}, b: {_id: "b0", x: 10}}],
});

const expected = [
    {
        "_id": {
            "_id": "first",
            "a": 1,
            "b": 1,
        },
        "a": {
            "_id": "first",
            "a": 1,
            "b": 1,
        },
        "b": {
            "_id": "first",
            "a": 1,
            "b": 1,
        },
    },
    {
        "_id": {
            "_id": "second",
            "a": 1,
            "b": 2,
        },
        "a": {
            "_id": "first",
            "a": 1,
            "b": 1,
        },
        "b": {
            "_id": "second",
            "a": 1,
            "b": 2,
        },
    },
];

assertArrayEq({actual, expected});
