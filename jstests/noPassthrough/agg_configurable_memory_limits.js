// Tests that certain aggregation operators have configurable memory limits.
(function() {
"use strict";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB("test");
const coll = db.agg_configurable_memory_limit;

const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < 100; i++) {
    bulk.insert({_id: i, x: i, y: ["string 1", "string 2", "string 3", "string 4", "string " + i]});
}
assert.commandWorked(bulk.execute());

// Test that pushing a bunch of strings to an array does not exceed the default 100MB memory limit.
assert.doesNotThrow(
    () => coll.aggregate([{$unwind: "$y"}, {$group: {_id: null, strings: {$push: "$y"}}}]));

// Now lower the limit to test that it's configuration is obeyed.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryMaxPushBytes: 100}));
assert.throwsWithCode(
    () => coll.aggregate([{$unwind: "$y"}, {$group: {_id: null, strings: {$push: "$y"}}}]),
    ErrorCodes.ExceededMemoryLimit);

// Test that using $addToSet behaves similarly.
assert.doesNotThrow(
    () => coll.aggregate([{$unwind: "$y"}, {$group: {_id: null, strings: {$addToSet: "$y"}}}]));

assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryMaxAddToSetBytes: 100}));
assert.throwsWithCode(
    () => coll.aggregate([{$unwind: "$y"}, {$group: {_id: null, strings: {$addToSet: "$y"}}}]),
    ErrorCodes.ExceededMemoryLimit);

const isExactTopNEnabled = db.adminCommand({getParameter: 1, featureFlagExactTopNAccumulator: 1})
                               .featureFlagExactTopNAccumulator.value;

if (isExactTopNEnabled) {
    // Capture the default value of 'internalQueryTopNAccumulatorBytes' to reset in between runs.
    const res = assert.commandWorked(
        db.adminCommand({getParameter: 1, internalQueryTopNAccumulatorBytes: 1}));
    const topNDefault = res["internalQueryTopNAccumulatorBytes"];

    // Test that the 'n' family of accumulators behaves similarly.
    for (const op of ["$firstN", "$lastN", "$minN", "$maxN", "$topN", "$bottomN"]) {
        let spec = {n: 200};

        // $topN/$bottomN both require a sort specification.
        if (op === "$topN" || op === "$bottomN") {
            spec["sortBy"] = {y: 1};
            spec["output"] = "$y";
        } else {
            // $firstN/$lastN/$minN/$maxN accept 'input'.
            spec["input"] = "$y";
        }

        // First, verify that the accumulator doesn't throw.
        db.adminCommand({setParameter: 1, internalQueryTopNAccumulatorBytes: topNDefault});
        assert.doesNotThrow(
            () => coll.aggregate([{$unwind: "$y"}, {$group: {_id: null, strings: {[op]: spec}}}]));

        // Then, verify that the memory limit throws when lowered.
        db.adminCommand({setParameter: 1, internalQueryTopNAccumulatorBytes: 100});
        assert.throwsWithCode(
            () => coll.aggregate([{$unwind: "$y"}, {$group: {_id: null, strings: {[op]: spec}}}]),
            ErrorCodes.ExceededMemoryLimit);
    }
}
MongoRunner.stopMongod(conn);
}());
