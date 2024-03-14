// Tests the ability of the $_internalJsEmit expression to access javascript scope explicitly
// specified in runtimeConstants.
//
// Do not run in sharded passthroughs since 'runtimeConstants' is disallowed on mongos.
// Must also set 'fromMongos: true' as otherwise 'runtimeConstants' is disallowed on mongod.
// @tags: [
//   assumes_against_mongod_not_mongos,
//   requires_scripting,
// ]
import {resultsEq} from "jstests/aggregation/extras/utils.js";

const coll = db.js_emit_with_scope;
coll.drop();

const weights = {
    wood: 5,
    chuck: 2,
    could: 0
};

let constants = {
    localNow: new Date(),
    clusterTime: new Timestamp(0, 0),
    jsScope: {weights: weights}
};

function fmap() {
    for (let word of this.text.split(' ')) {
        emit(word, weights[word]);
    }
}

let pipeline = [
    {
        $project: {
            emits: {
                $_internalJsEmit: {
                    this: '$$ROOT',
                    eval: fmap,
                },
            },
            _id: 0,
        }
    },
    {$unwind: '$emits'},
    {$replaceRoot: {newRoot: '$emits'}}
];

assert.commandWorked(coll.insert({text: 'wood chuck could chuck wood'}));

let results =
    coll.aggregate(pipeline, {cursor: {}, runtimeConstants: constants, fromMongos: true}).toArray();
assert(resultsEq(results,
                 [
                     {k: "wood", v: weights["wood"]},
                     {k: "chuck", v: weights["chuck"]},
                     {k: "could", v: weights["could"]},
                     {k: "chuck", v: weights["chuck"]},
                     {k: "wood", v: weights["wood"]}
                 ],
                 results));

//
// Test that the scope variables are mutable from within a user-defined javascript function.
//
pipeline[0].$project.emits.$_internalJsEmit.eval = function() {
    for (let word of this.text.split(' ')) {
        emit(word, weights[word]);
        weights[word] += 1;
    }
};

results =
    coll.aggregate(pipeline, {cursor: {}, runtimeConstants: constants, fromMongos: true}).toArray();
assert(resultsEq(results,
                 [
                     {k: "wood", v: weights["wood"]},
                     {k: "chuck", v: weights["chuck"]},
                     {k: "could", v: weights["could"]},
                     {k: "chuck", v: weights["chuck"] + 1},
                     {k: "wood", v: weights["wood"] + 1}
                 ],
                 results));

//
// Test that the jsScope is allowed to have any number of fields.
//
/* eslint-disable */
constants.jsScope.multiplier = 5;
pipeline[0].$project.emits.$_internalJsEmit.eval = function() {
    for (let word of this.text.split(' ')) {
        emit(word, weights[word] * multiplier);
    }
};
/* eslint-enable */

results =
    coll.aggregate(pipeline, {cursor: {}, runtimeConstants: constants, fromMongos: true}).toArray();
assert(resultsEq(results,
                 [
                     {k: "wood", v: weights["wood"] * 5},
                     {k: "chuck", v: weights["chuck"] * 5},
                     {k: "could", v: weights["could"] * 5},
                     {k: "chuck", v: weights["chuck"] * 5},
                     {k: "wood", v: weights["wood"] * 5}
                 ],
                 results));
constants.jsScope = {};
pipeline[0].$project.emits.$_internalJsEmit.eval = function() {
    for (let word of this.text.split(' ')) {
        emit(word, 1);
    }
};
results =
    coll.aggregate(pipeline, {cursor: {}, runtimeConstants: constants, fromMongos: true}).toArray();
assert(resultsEq(results,
                 [
                     {k: "wood", v: 1},
                     {k: "chuck", v: 1},
                     {k: "could", v: 1},
                     {k: "chuck", v: 1},
                     {k: "wood", v: 1},
                 ],
                 results));

//
// Test that the command fails if the jsScope is not an object.
//
constants.jsScope = "you cant do this";
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: pipeline,
    cursor: {},
    runtimeConstants: constants,
    fromMongos: true
}),
                             ErrorCodes.TypeMismatch);
