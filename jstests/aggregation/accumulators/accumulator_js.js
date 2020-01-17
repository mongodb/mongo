// Test the behavior of user-defined (Javascript) accumulators.
(function() {
"use strict";

load('jstests/aggregation/extras/utils.js');

db.accumulator_js.drop();

for (const word of ["hello", "world", "world", "hello", "hi"]) {
    db.accumulator_js.insert({word: word, val: 1});
}

const command = {
    aggregate: 'accumulator_js',
    cursor: {},
    pipeline: [{
        $group: {
            _id: "$word",
            wordCount: {
                $accumulator: {
                    init: function() {
                        return 0;
                    },
                    accumulateArgs: ["$val"],
                    accumulate: function(state, val) {
                        return state + val;
                    },
                    merge: function(state1, state2) {
                        return state1 + state2;
                    },
                    finalize: function(state) {
                        return state;
                    }
                }
            }
        }
    }],
};

const expectedResults = [
    {_id: "hello", wordCount: 2},
    {_id: "world", wordCount: 2},
    {_id: "hi", wordCount: 1},
];

let res = assert.commandWorked(db.runCommand(command));
assert(resultsEq(res.cursor.firstBatch, expectedResults), res.cursor);

// Test that the functions can be passed as strings.
{
    const accumulatorSpec = command.pipeline[0].$group.wordCount.$accumulator;
    accumulatorSpec.init = accumulatorSpec.init.toString();
    accumulatorSpec.accumulate = accumulatorSpec.accumulate.toString();
    accumulatorSpec.merge = accumulatorSpec.merge.toString();
    accumulatorSpec.finalize = accumulatorSpec.finalize.toString();
}
res = assert.commandWorked(db.runCommand(command));
assert(resultsEq(res.cursor.firstBatch, expectedResults), res.cursor);

// Test that finalize is optional.
delete command.pipeline[0].$group.wordCount.$accumulator.finalize;
res = assert.commandWorked(db.runCommand(command));
assert(resultsEq(res.cursor.firstBatch, expectedResults), res.cursor);

// Test a finalizer other than the identity function. Finalizers are useful when the intermediate
// state needs to be a different format from the final result.
res = assert.commandWorked(db.runCommand(Object.merge(command, {
    pipeline: [{
        $group: {
            _id: 1,
            avgWordLen: {
                $accumulator: {
                    init: function() {
                        return {count: 0, sum: 0};
                    },
                    accumulateArgs: [{$strLenCP: "$word"}],
                    accumulate: function({count, sum}, wordLen) {
                        return {count: count + 1, sum: sum + wordLen};
                    },
                    merge: function(s1, s2) {
                        return {count: s1.count + s2.count, sum: s1.sum + s2.sum};
                    },
                    finalize: function({count, sum}) {
                        return sum / count;
                    },
                    lang: 'js',
                }
            },
        }
    }],
})));
assert(resultsEq(res.cursor.firstBatch, [{_id: 1, avgWordLen: 22 / 5}]), res.cursor);

// Test that a null word is considered a valid value.
assert.commandWorked(db.accumulator_js.insert({word: null, val: 1}));
expectedResults.push({_id: null, wordCount: 1});
res = assert.commandWorked(db.runCommand(command));
assert(resultsEq(res.cursor.firstBatch, expectedResults), res.cursor);

// Test that missing fields become JS null.
// This is similar to how most other agg operators work.
// TODO SERVER-45450 is this a problem for mapreduce?
assert(db.accumulator_js.drop());
assert.commandWorked(db.accumulator_js.insert({sentinel: 1}));
command.pipeline = [{
    $group: {
        _id: 1,
        value: {
            $accumulator: {
                init: function() {
                    return [];
                },
                accumulateArgs: ["$no_such_field"],
                accumulate: function(state, value) {
                    return state.concat([value]);
                },
                merge: function(s1, s2) {
                    return s1.concat(s2);
                },
                lang: 'js',
            }
        }
    }
}];
res = assert.commandWorked(db.runCommand(command));
assert(resultsEq(res.cursor.firstBatch, [{_id: 1, value: [null]}]), res.cursor);
})();
