// Test several different kinds of size limits on user-defined (Javascript) accumulators.
(function() {
"use strict";

const coll = db.accumulator_js_size_limits;

function runExample(groupKey, accumulatorSpec) {
    return coll.runCommand({
        aggregate: coll.getName(),
        cursor: {},
        pipeline: [{
            $group: {
                _id: groupKey,
                accumulatedField: {$accumulator: accumulatorSpec},
            }
        }]
    });
}

// Accumulator tries to create too long a String; it can't be serialized to BSON.
coll.drop();
assert.commandWorked(coll.insert({}));
let res = runExample(1, {
    init: function() {
        return "a".repeat(20 * 1024 * 1024);
    },
    accumulate: function() {
        throw 'accumulate should not be called';
    },
    accumulateArgs: [],
    merge: function() {
        throw 'merge should not be called';
    },
    finalize: function() {
        throw 'finalize should not be called';
    },
    lang: 'js',
});
assert.commandFailedWithCode(res, [10334]);

// Accumulator tries to return BSON larger than 16MB from JS.
assert(coll.drop());
assert.commandWorked(coll.insert({}));
res = runExample(1, {
    init: function() {
        const str = "a".repeat(1 * 1024 * 1024);
        return Array.from({length: 20}, () => str);
    },
    accumulate: function() {
        throw 'accumulate should not be called';
    },
    accumulateArgs: [],
    merge: function() {
        throw 'merge should not be called';
    },
    finalize: function() {
        throw 'finalize should not be called';
    },
    lang: 'js',
});
assert.commandFailedWithCode(res, [17260]);

// Accumulator state and argument together exceed max BSON size.
assert(coll.drop());
const oneMBString = "a".repeat(1 * 1024 * 1024);
const tenMBArray = Array.from({length: 10}, () => oneMBString);
assert.commandWorked(coll.insert([{arr: tenMBArray}, {arr: tenMBArray}]));
res = runExample(1, {
    init: function() {
        return [];
    },
    accumulate: function(state, input) {
        state.push(input);
        return state;
    },
    accumulateArgs: ["$arr"],
    merge: function(state1, state2) {
        return state1.concat(state2);
    },
    finalize: function() {
        throw 'finalize should not be called';
    },
    lang: 'js',
});
assert.commandFailedWithCode(res, [4545000]);

// $group size limit exceeded, and cannot spill.
assert(coll.drop());
assert.commandWorked(coll.insert(Array.from({length: 200}, (_, i) => ({_id: i}))));
// By grouping on _id, each group contains only 1 document. This means it creates many
// AccumulatorState instances.
res = runExample("$_id", {
    init: function() {
        // Each accumulator state is big enough to be expensive, but not big enough to hit the BSON
        // size limit.
        return "a".repeat(1 * 1024 * 1024);
    },
    accumulate: function(state) {
        return state;
    },
    accumulateArgs: [1],
    merge: function(state1, state2) {
        return state1;
    },
    finalize: function(state) {
        return state.length;
    },
    lang: 'js',
});
assert.commandFailedWithCode(res, [ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed]);
})();
