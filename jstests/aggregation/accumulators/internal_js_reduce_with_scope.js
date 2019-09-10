// Tests the ability of the $_internalJsReduce accumulator to access javascript scope explicitly
// specified in runtimeConstants.
//
// Do not run in sharded passthroughs since 'runtimeConstants' is disallowed on mongos.
// @tags: [assumes_unsharded_collection]
(function() {
"use strict";

load('jstests/aggregation/extras/utils.js');

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
};

const expectedResults = [
    {_id: "hello", wordCountMod: 2},
    {_id: "world", wordCountMod: 0},
    {_id: "hi", wordCountMod: 1},
];

const res = assert.commandWorked(db.runCommand(command));
assert(resultsEq(res.cursor.firstBatch, expectedResults, res.cursor));
})();
