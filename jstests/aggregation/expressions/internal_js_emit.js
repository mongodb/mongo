// Tests basic functionality of the $_internalJsEmit expression, which provides capability for the
// map stage of MapReduce.
(function() {
"use strict";

load('jstests/aggregation/extras/utils.js');

const coll = db.js_emit_expr;
coll.drop();

function fmap() {
    for (let word of this.text.split(' ')) {
        emit(word, 1);
    }
}

let pipeline = [
    {
        $project: {
            emits: {
                $_internalJsEmit: {
                    'this': '$$ROOT',
                    'eval': fmap,
                },
            },
            _id: 0,
        }
    },
    {$unwind: '$emits'},
    {$replaceRoot: {newRoot: '$emits'}}
];

assert.commandWorked(coll.insert({text: 'hello world'}));

let results = coll.aggregate(pipeline, {cursor: {batchSize: 1}}).toArray();
assert(resultsEq(results, [{k: "hello", v: 1}, {k: "world", v: 1}]), results);

assert.commandWorked(coll.insert({text: 'mongo db'}));

// Set batchSize to 1 to check that the expression is able to run across getMore's.
results = coll.aggregate(pipeline, {cursor: {batchSize: 1}}).toArray();
assert(resultsEq(results,
                 [{k: 'hello', v: 1}, {k: 'world', v: 1}, {k: 'mongo', v: 1}, {k: 'db', v: 1}]),
       results);

// Test that the 'eval' function accepts a string argument.
pipeline[0].$project.emits.$_internalJsEmit.eval = fmap.toString();
results = coll.aggregate(pipeline, {cursor: {batchSize: 1}}).toArray();
assert(resultsEq(results,
                 [{k: 'hello', v: 1}, {k: 'world', v: 1}, {k: 'mongo', v: 1}, {k: 'db', v: 1}]),
       results);

// Test that the command correctly fails for an invalid operation within the JS function.
assert.commandWorked(coll.insert({text: 5}));
assert.commandFailedWithCode(
    db.runCommand({aggregate: coll.getName(), pipeline: pipeline, cursor: {}}),
    ErrorCodes.JSInterpreterFailure);

// Test that the command correctly fails for invalid arguments.
pipeline = [{
    $project: {
        emits: {
            $_internalJsEmit: {
                'this': 'must evaluate to an object',
                'eval': fmap,
            },
        },
        _id: 0,
    }
}];
assert.commandFailedWithCode(
    db.runCommand({aggregate: coll.getName(), pipeline: pipeline, cursor: {}}), 31225);

pipeline = [{
    $project: {
        emits: {
            $_internalJsEmit: {
                'this': '$$ROOT',
                'eval': 'this is not a valid function!',
            },
        },
        _id: 0,
    }
}];
assert.commandFailedWithCode(
    db.runCommand({aggregate: coll.getName(), pipeline: pipeline, cursor: {}}),
    ErrorCodes.JSInterpreterFailure);
})();