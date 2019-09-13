// Tests basic functionality of the $_internalJsReduce accumulator, which provides capability for
// the reduce stage of MapReduce.
(function() {
"use strict";

load('jstests/aggregation/extras/utils.js');

db.js_reduce.drop();

for (const i of ["hello", "world", "world", "hello", "hi"]) {
    db.js_reduce.insert({word: i, val: 1});
}

// Simple reduce function which calculates the word count.
function reduce(key, values) {
    return Array.sum(values);
}

let groupPipe = [{
    $group: {
        _id: "$word",
        wordCount: {
            $_internalJsReduce: {
                data: {k: "$word", v: "$val"},
                eval: reduce,
            }
        }
    }
}];

let command = {
    aggregate: 'js_reduce',
    cursor: {},
    pipeline: groupPipe,
    allowDiskUse: true  // Set allowDiskUse to true to force the expression to run on a shard in the
                        // passthrough suites, where javascript execution is supported.
};

const expectedResults = [
    {_id: "hello", wordCount: 2},
    {_id: "world", wordCount: 2},
    {_id: "hi", wordCount: 1},
];

let res = assert.commandWorked(db.runCommand(command));
assert(resultsEq(res.cursor.firstBatch, expectedResults, res.cursor));

//
// Test that the reduce function also accepts a string argument.
//
groupPipe[0].$group.wordCount.$_internalJsReduce.eval = reduce.toString();
res = assert.commandWorked(db.runCommand(command));
assert(resultsEq(res.cursor.firstBatch, expectedResults), res.cursor);

//
// Test that a null word is considered a valid value.
//
assert.commandWorked(db.js_reduce.insert({word: null, val: 1}));
expectedResults.push({_id: null, wordCount: 1});
res = assert.commandWorked(db.runCommand(command));
assert(resultsEq(res.cursor.firstBatch, expectedResults), res.cursor);

//
// Test that the command fails for a missing key and/or value.
//
assert.commandWorked(db.js_reduce.insert({sentinel: 1}));
assert.commandFailedWithCode(db.runCommand(command), 31251);
assert.commandWorked(db.js_reduce.remove({sentinel: 1}));

//
// Test that the accumulator fails if the size of the accumulated values exceeds the internal BSON
// limit.
//
db.js_reduce.drop();
const longStringLength = 1 * 1024 * 1024;
const nDocs = 20;
let bulk = db.js_reduce.initializeUnorderedBulkOp();
for (let i = 0; i < nDocs; i++) {
    bulk.insert({word: "hello", val: "a".repeat(longStringLength)});
}
assert.commandWorked(bulk.execute());
assert.commandFailedWithCode(db.runCommand(command), [ErrorCodes.BSONObjectTooLarge, 16493]);

//
// Test that the accumulator correctly fails for invalid arguments.
//
db.js_reduce.drop();
assert.commandWorked(db.js_reduce.insert({word: "oi", val: 1}));
assert.commandWorked(db.js_reduce.insert({word: "oi", val: 2}));
groupPipe[0].$group.wordCount.$_internalJsReduce.eval = "UDFs are great!";
assert.commandFailedWithCode(db.runCommand(command), ErrorCodes.JSInterpreterFailure);

groupPipe[0].$group.wordCount.$_internalJsReduce.eval = 5;
assert.commandFailedWithCode(db.runCommand(command), 31244);

groupPipe[0].$group.wordCount.$_internalJsReduce.eval = reduce;
groupPipe[0].$group.wordCount.$_internalJsReduce.data = 5;
assert.commandFailedWithCode(db.runCommand(command), 31245);

groupPipe[0].$group.wordCount.$_internalJsReduce = {
    notEval: 1,
    notData: 1
};
assert.commandFailedWithCode(db.runCommand(command), 31243);

groupPipe[0].$group.wordCount.$_internalJsReduce = {
    eval: reduce,
    data: {v: 1}
};
assert.commandFailedWithCode(db.runCommand(command), 31251);
groupPipe[0].$group.wordCount.$_internalJsReduce.data = {
    key: 1,
    value: 1
};
assert.commandFailedWithCode(db.runCommand(command), 31251);
})();
