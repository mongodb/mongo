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

for (const i of ["hello", "world", "world", "hello", "hi"]) {
    assert.commandWorked(coll.insert({word: i, val: 1}));
}

// Simple reduce function which calculates the word value based on weights defined in a local JS
// variable.
const weights = {
    hello: 3,
    world: 2,
    hi: 1
};
function reduce(key, values) {
    return Array.sum(values) * weights[key];
}

const command = {
    aggregate: coll.getName(),
    cursor: {},
    runtimeConstants:
        {localNow: new Date(), clusterTime: new Timestamp(0, 0), jsScope: {weights: weights}},
    pipeline: [{
        $group: {
            _id: "$word",
            wordCount: {
                $_internalJsReduce: {
                    data: {k: "$word", v: "$val"},
                    eval: reduce,
                }
            }
        }
    }],
};

const expectedResults = [
    {_id: "hello", wordCount: 6},
    {_id: "world", wordCount: 4},
    {_id: "hi", wordCount: 1},
];

const res = assert.commandWorked(db.runCommand(command));
assert(resultsEq(res.cursor.firstBatch, expectedResults, res.cursor));
})();
