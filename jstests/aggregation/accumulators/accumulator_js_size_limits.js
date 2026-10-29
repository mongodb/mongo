// Test several different kinds of size limits on user-defined (Javascript) accumulators.
// @tags: [
//   requires_scripting,
//   resource_intensive,
// ]
import {isMozjsWasm} from "jstests/libs/js_engine_util.js";

const coll = db.accumulator_js_size_limits;

function runExample(groupKey, accumulatorSpec, aggregateOptions = {}) {
    const aggregateCmd = {
        aggregate: coll.getName(),
        cursor: {},
        pipeline: [
            {
                $group: {
                    _id: groupKey,
                    accumulatedField: {$accumulator: accumulatorSpec},
                },
            },
        ],
    };
    return coll.runCommand(Object.assign(aggregateCmd, aggregateOptions));
}

// BSONObjMaxUserSize (16 MB) from the server; BSONObjMaxInternalSize adds 16 KB of internal
// overhead headroom on top of the user-facing limit (see bson/util/builder.h).
const maxBsonOverhead = db.hello().maxBsonObjectSize + 16 * 1024;

// Accumulator tries to create too long a String; it can't be serialized to BSON.
// WASM: allocate just 1 byte over BSONObjMaxInternalSize so the BSON-size check fires
// without requiring 20 MB of WASM linear memory.
// Legacy: use the original 20 MB string.
const tooBigStringLen = isMozjsWasm(db) ? maxBsonOverhead + 1 : 20 * 1024 * 1024;
coll.drop();
assert.commandWorked(coll.insert({}));
let res = runExample(1, {
    init: `function() { return "a".repeat(${tooBigStringLen}); }`,
    accumulate: function () {
        throw "accumulate should not be called";
    },
    accumulateArgs: [],
    merge: function () {
        throw "merge should not be called";
    },
    finalize: function () {
        throw "finalize should not be called";
    },
    lang: "js",
});
// WASM path: objectwrapper.cpp throws 17260 ("Object size exceeds limit").
// Legacy MozJS path: may throw 10334 (BSONObjectTooLarge) from BSONObjBuilder.
assert.commandFailedWithCode(res, [ErrorCodes.BSONObjectTooLarge, 17260, 10334]);

// Accumulator tries to return BSON larger than 16MB from JS.
// WASM: use 2 strings of (BSONObjMaxInternalSize / 2 + 1) bytes each — BSON serialization
// exceeds BSONObjMaxInternalSize with ~16 MB peak live JS memory, within the WASM store limiter.
// Legacy: use the original 20 × 1 MB strings.
const [tooBigArrayLen, tooBigArrayElemLen] = isMozjsWasm(db)
    ? [2, maxBsonOverhead / 2 + 1]
    : [20, 1 * 1024 * 1024];
assert(coll.drop());
assert.commandWorked(coll.insert({}));
res = runExample(1, {
    init: `function() {
        const str = "a".repeat(${tooBigArrayElemLen});
        return Array.from({length: ${tooBigArrayLen}}, () => str);
    }`,
    accumulate: function () {
        throw "accumulate should not be called";
    },
    accumulateArgs: [],
    merge: function () {
        throw "merge should not be called";
    },
    finalize: function () {
        throw "finalize should not be called";
    },
    lang: "js",
});
assert.commandFailedWithCode(res, [17260, 10334]);

// Accumulator state and argument together exceed max BSON size.
assert(coll.drop());
const oneMBString = "a".repeat(1 * 1024 * 1024);
const tenMBArray = Array.from({length: 10}, () => oneMBString);
assert.commandWorked(coll.insert([{arr: tenMBArray}, {arr: tenMBArray}]));
res = runExample(1, {
    init: function () {
        return [];
    },
    accumulate: function (state, input) {
        state.push(input);
        return state;
    },
    accumulateArgs: ["$arr"],
    merge: function (state1, state2) {
        return state1.concat(state2);
    },
    finalize: function () {
        throw "finalize should not be called";
    },
    lang: "js",
});
// 4545000 is thrown by accumulator_js_reduce.cpp on the server before JS runs,
// so it surfaces identically on legacy and WASM builds.
assert.commandFailedWithCode(res, [4545000]);

// $group size limit exceeded, and cannot spill.
assert(coll.drop());
assert.commandWorked(coll.insert(Array.from({length: 200}, (_, i) => ({_id: i}))));
// By grouping on _id, each group contains only 1 document. This means it creates many
// AccumulatorState instances.
res = runExample(
    "$_id",
    {
        init: function () {
            // Each accumulator state is big enough to be expensive, but not big enough
            // to hit the BSON size limit.
            return "a".repeat(1 * 1024 * 1024);
        },
        accumulate: function (state) {
            return state;
        },
        accumulateArgs: [1],
        merge: function (state1, state2) {
            return state1;
        },
        finalize: function (state) {
            return state.length;
        },
        lang: "js",
    },
    {allowDiskUse: false},
);
// If featureFlagShardFilteringDistinctScan is on, we will push this $group down to shards on
// sharded collection passthrough suites, and may run out of space during JS execution of init().
assert.commandFailedWithCode(res, [
    ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed,
    ErrorCodes.JSInterpreterFailure,
]);

// Verify that having large number of documents doesn't cause the $accumulator to run out of memory.
coll.drop();
assert.commandWorked(coll.insert({groupBy: 1, largeField: "a".repeat(1000)}));
assert.commandWorked(coll.insert({groupBy: 2, largeField: "a".repeat(1000)}));
const largeAccumulator = {
    $accumulator: {
        init: function () {
            return "";
        },
        accumulateArgs: [{fieldName: "$a"}],
        accumulate: function (state, args) {
            return state + "a";
        },
        merge: function (state1, state2) {
            return state1 + state2;
        },
        finalize: function (state) {
            return state.length;
        },
    },
};
res = coll
    .aggregate([
        {$addFields: {a: {$range: [0, 250000]}}},
        {$unwind: "$a"}, // Create a number of documents to be executed by the accumulator.
        {$group: {_id: "$groupBy", count: largeAccumulator}},
    ])
    .toArray();
assert.sameMembers(res, [
    {_id: 1, count: 250000},
    {_id: 2, count: 250000},
]);

// With $bucket.
res = coll
    .aggregate([
        {$addFields: {a: {$range: [0, 250000]}}},
        {$unwind: "$a"}, // Create a number of documents to be executed by the accumulator.
        {
            $bucket: {
                groupBy: "$groupBy",
                boundaries: [1, 2, 3],
                output: {count: largeAccumulator},
            },
        },
    ])
    .toArray();
assert.sameMembers(res, [
    {_id: 1, count: 250000},
    {_id: 2, count: 250000},
]);
