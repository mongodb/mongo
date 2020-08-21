// Test several different kinds of size limits on user-defined (Javascript) accumulators.
// @tags: [resource_intensive]
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

// Verify that having large number of documents doesn't cause the $accumulator to run out of memory.
coll.drop();
assert.commandWorked(coll.insert({groupBy: 1, largeField: "a".repeat(1000)}));
assert.commandWorked(coll.insert({groupBy: 2, largeField: "a".repeat(1000)}));
const largeAccumulator = {
    $accumulator: {
        init: function() {
            return "";
        },
        accumulateArgs: [{fieldName: "$a"}],
        accumulate: function(state, args) {
            return state + "a";
        },
        merge: function(state1, state2) {
            return state1 + state2;
        },
        finalize: function(state) {
            return state.length;
        }
    }
};
res = coll.aggregate([
              {$addFields: {a: {$range: [0, 1000000]}}},
              {$unwind: "$a"},  // Create a number of documents to be executed by the accumulator.
              {$group: {_id: "$groupBy", count: largeAccumulator}}
          ])
          .toArray();
assert.sameMembers(res, [{_id: 1, count: 1000000}, {_id: 2, count: 1000000}]);

// With $bucket.
res =
    coll.aggregate([
            {$addFields: {a: {$range: [0, 1000000]}}},
            {$unwind: "$a"},  // Create a number of documents to be executed by the accumulator.
            {
                $bucket:
                    {groupBy: "$groupBy", boundaries: [1, 2, 3], output: {count: largeAccumulator}}
            }
        ])
        .toArray();
assert.sameMembers(res, [{_id: 1, count: 1000000}, {_id: 2, count: 1000000}]);
})();
