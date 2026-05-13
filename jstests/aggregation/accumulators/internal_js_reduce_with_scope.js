// Tests the ability of the $_internalJsReduce accumulator to access javascript scope explicitly
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

const coll = db.js_reduce_with_scope;
coll.drop();

for (const i of ["hello", "world", "world", "world", "hello", "hi", "hi", "hi", "hi"]) {
    assert.commandWorked(coll.insert({word: i, val: 1}));
}

// Simple reduce function which calculates the word count and mods it based on a value defined in a
// local JS variable.
const modulus = 3;
function reduce(key, values) {
    return Array.sum(values) % modulus;
}

const command = {
    aggregate: coll.getName(),
    cursor: {},
    runtimeConstants:
        {localNow: new Date(), clusterTime: new Timestamp(0, 0), jsScope: {modulus: modulus}},
    pipeline: [{
        $group: {
            _id: "$word",
            wordCountMod: {
                $_internalJsReduce: {
                    data: {k: "$word", v: "$val"},
                    eval: reduce,
                }
            }
        }
    }],
    fromMongos: true,
    readConcern: {},
    writeConcern: {},
};

const expectedResults = [
    {_id: "hello", wordCountMod: 2},
    {_id: "world", wordCountMod: 0},
    {_id: "hi", wordCountMod: 1},
];

const internalConn = new Mongo(db.getMongo().host);
assert.commandWorked(
    internalConn.getDB("admin").runCommand({
        hello: 1,
        internalClient: {minWireVersion: NumberInt(0), maxWireVersion: NumberInt(7)},
    }),
);
const internalDB = internalConn.getDB(db.getName());

const res = assert.commandWorked(internalDB.runCommand(command));
assert(resultsEq(res.cursor.firstBatch, expectedResults, res.cursor));
