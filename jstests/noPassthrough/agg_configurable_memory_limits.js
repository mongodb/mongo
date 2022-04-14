// Tests that certain aggregation operators have configurable memory limits.
(function() {
"use strict";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB("test");
const coll = db.agg_configurable_memory_limit;

// The approximate size of the strings below is 22-25 bytes, so configure one memory limit such that
// 100 of these strings will surely exceed it but 24 of them won't, and another such that 24 will
// exceed as well.
const stringSize = 22;
const nSetBaseline = 20;
const memLimitArray = nSetBaseline * 5 * stringSize / 2;
const memLimitSet = (nSetBaseline + 4) * stringSize / 2;

const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < nSetBaseline; i++) {
    bulk.insert({_id: 5 * i + 0, y: "string 0"});
    bulk.insert({_id: 5 * i + 1, y: "string 1"});
    bulk.insert({_id: 5 * i + 2, y: "string 2"});
    bulk.insert({_id: 5 * i + 3, y: "string 3"});
    bulk.insert({_id: 5 * i + 4, y: "string " + 5 * i + 4});
}
assert.commandWorked(bulk.execute());

(function testInternalQueryMaxPushBytesSetting() {
    // Test that the default 100MB memory limit isn't reached with our data.
    assert.doesNotThrow(() => coll.aggregate([{$group: {_id: null, strings: {$push: "$y"}}}]));

    // Now lower the limit to test that it's configuration is obeyed.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryMaxPushBytes: memLimitArray}));
    assert.throwsWithCode(() => coll.aggregate([{$group: {_id: null, strings: {$push: "$y"}}}]),
                          ErrorCodes.ExceededMemoryLimit);
}());

(function testInternalQueryMaxAddToSetBytesSetting() {
    // Test that the default 100MB memory limit isn't reached with our data.
    assert.doesNotThrow(() => coll.aggregate([{$group: {_id: null, strings: {$addToSet: "$y"}}}]));

    // Test that $addToSet needs a tighter limit than $push (because some of the strings are the
    // same).
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryMaxAddToSetBytes: memLimitArray}));
    assert.doesNotThrow(() => coll.aggregate([{$group: {_id: null, strings: {$addToSet: "$y"}}}]));

    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryMaxAddToSetBytes: memLimitSet}));
    assert.throwsWithCode(() => coll.aggregate([{$group: {_id: null, strings: {$addToSet: "$y"}}}]),
                          ErrorCodes.ExceededMemoryLimit);
}());

(function testInternalQueryTopNAccumulatorBytesSetting() {
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
        assert.doesNotThrow(() => coll.aggregate([{$group: {_id: null, strings: {[op]: spec}}}]));

        // Then, verify that the memory limit throws when lowered.
        db.adminCommand({setParameter: 1, internalQueryTopNAccumulatorBytes: 100});
        assert.throwsWithCode(() => coll.aggregate([{$group: {_id: null, strings: {[op]: spec}}}]),
                              ErrorCodes.ExceededMemoryLimit);
    }
}());

MongoRunner.stopMongod(conn);
}());
