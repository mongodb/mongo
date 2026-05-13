// Tests the ability of the $_internalJsEmit expression to access javascript scope explicitly
// specified in runtimeConstants.
//
// Do not run in sharded passthroughs since 'runtimeConstants' is disallowed on mongos.
// Must also set 'fromMongos: true' as otherwise 'runtimeConstants' is disallowed on mongod.
// @tags: [
//   assumes_against_mongod_not_mongos,
//   requires_scripting,
//   # Uses fromMongos: true which requires an internal client connection; secondary-reads
//   # passthroughs route commands through non-internal connections and break this.
//   requires_spawning_own_processes,
//   # Uses fromMongos: true with runtimeConstants which requires internalClient; not compatible
//   # with FCV upgrade/downgrade suites that may restart nodes mid-test.
//   cannot_run_during_upgrade_downgrade,
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

const internalConn = new Mongo(db.getMongo().host);
assert.commandWorked(
    internalConn.getDB("admin").runCommand({
        hello: 1,
        internalClient: {minWireVersion: NumberInt(0), maxWireVersion: NumberInt(7)},
    }),
);
const internalDB = internalConn.getDB(db.getName());
const internalColl = internalDB[coll.getName()];

let results = internalColl
              .aggregate(pipeline, {
                  cursor: {},
                  runtimeConstants: constants,
                  fromMongos: true,
                  readConcern: {},
                  writeConcern: {},
              })
              .toArray();
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

results = internalColl
              .aggregate(pipeline, {
                  cursor: {},
                  runtimeConstants: constants,
                  fromMongos: true,
                  readConcern: {},
                  writeConcern: {},
              })
              .toArray();
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

constants.jsScope.multiplier = 5;
pipeline[0].$project.emits.$_internalJsEmit.eval = function() {
    for (let word of this.text.split(' ')) {
        emit(word, weights[word] * multiplier);
    }
};

results = internalColl
              .aggregate(pipeline, {
                  cursor: {},
                  runtimeConstants: constants,
                  fromMongos: true,
                  readConcern: {},
                  writeConcern: {},
              })
              .toArray();
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
results = internalColl
              .aggregate(pipeline, {
                  cursor: {},
                  runtimeConstants: constants,
                  fromMongos: true,
                  readConcern: {},
                  writeConcern: {},
              })
              .toArray();
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
assert.commandFailedWithCode(
    internalDB.runCommand({
        aggregate: coll.getName(),
        pipeline: pipeline,
        cursor: {},
        runtimeConstants: constants,
        fromMongos: true,
        readConcern: {},
        writeConcern: {},
    }),
    ErrorCodes.TypeMismatch);
