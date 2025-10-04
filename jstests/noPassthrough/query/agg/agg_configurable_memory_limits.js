// Tests that certain aggregation operators have configurable memory limits.
const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB("test");
const coll = db.agg_configurable_memory_limit;

// Function to change the parameter value and return the previous value.
function setParam(param, val) {
    const res = db.adminCommand({setParameter: 1, [param]: val});
    assert.commandWorked(res);
    return res.was;
}

// The approximate size of the strings below is 22-25 bytes, so configure one memory limit such that
// 100 of these strings will surely exceed it but 24 of them won't, and another such that 24 will
// exceed as well.
const stringSize = 22;
const nSetBaseline = 20;
const memLimitArray = (nSetBaseline * 5 * stringSize) / 2;
const memLimitSet = ((nSetBaseline + 4) * stringSize) / 2;

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

    // Now lower the limit to test that its configuration is obeyed.
    const originalVal = setParam("internalQueryMaxPushBytes", memLimitArray);
    assert.throwsWithCode(
        () => coll.aggregate([{$group: {_id: null, strings: {$push: "$y"}}}]),
        ErrorCodes.ExceededMemoryLimit,
    );
    setParam("internalQueryMaxPushBytes", originalVal);
})();

(function testInternalQueryMaxPushBytesSettingWindowFunc() {
    let pipeline = [{$setWindowFields: {sortBy: {_id: 1}, output: {v: {$push: "$y"}}}}];
    // Test that the default 100MB memory limit isn't reached with our data.
    assert.doesNotThrow(() => coll.aggregate(pipeline));

    // Now lower the limit to test that its configuration is obeyed.
    const originalVal = setParam("internalQueryMaxPushBytes", memLimitArray);
    assert.throwsWithCode(() => coll.aggregate(pipeline), ErrorCodes.ExceededMemoryLimit);
    setParam("internalQueryMaxPushBytes", originalVal);
})();

(function testInternalQueryMaxAddToSetBytesSetting() {
    // Test that the default 100MB memory limit isn't reached with our data.
    assert.doesNotThrow(() => coll.aggregate([{$group: {_id: null, strings: {$addToSet: "$y"}}}]));

    // Test that $addToSet needs a tighter limit than $push (because some of the strings are the
    // same).
    const originalVal = setParam("internalQueryMaxAddToSetBytes", memLimitArray);
    assert.doesNotThrow(() => coll.aggregate([{$group: {_id: null, strings: {$addToSet: "$y"}}}]));

    setParam("internalQueryMaxAddToSetBytes", memLimitSet);
    assert.throwsWithCode(
        () => coll.aggregate([{$group: {_id: null, strings: {$addToSet: "$y"}}}]),
        ErrorCodes.ExceededMemoryLimit,
    );
    setParam("internalQueryMaxAddToSetBytes", originalVal);
})();

(function testInternalQueryMaxAddToSetBytesSettingWindowFunc() {
    let pipeline = [{$setWindowFields: {sortBy: {_id: 1}, output: {v: {$addToSet: "$y"}}}}];
    // Test that the default 100MB memory limit isn't reached with our data.
    assert.doesNotThrow(() => coll.aggregate(pipeline));

    // Test that $addToSet needs a tighter limit than $concatArrays (because some of the strings are
    // the same).
    const originalVal = setParam("internalQueryMaxAddToSetBytes", memLimitArray);
    assert.doesNotThrow(() => coll.aggregate(pipeline));

    // Test that a tighter limit for $addToSet is obeyed.
    setParam("internalQueryMaxAddToSetBytes", memLimitSet);
    assert.throwsWithCode(() => coll.aggregate(pipeline), ErrorCodes.ExceededMemoryLimit);
    setParam("internalQueryMaxAddToSetBytes", originalVal);
})();

(function testInternalQueryTopNAccumulatorBytesSetting() {
    // Capture the default value of 'internalQueryTopNAccumulatorBytes' to reset in between runs.
    const res = assert.commandWorked(db.adminCommand({getParameter: 1, internalQueryTopNAccumulatorBytes: 1}));
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
        assert.throwsWithCode(
            () => coll.aggregate([{$group: {_id: null, strings: {[op]: spec}}}]),
            ErrorCodes.ExceededMemoryLimit,
        );
    }
})();

(function testInternalQueryMaxMapReduceExpressionBytesSetting() {
    const str = "a".repeat(100);
    const mapPipeline = [{$project: {a: {$map: {input: {$range: [0, 100]}, in: str}}}}];
    const reducePipeline = [
        {
            $project: {
                a: {$reduce: {input: {$range: [0, 100]}, initialValue: [], in: {$concatArrays: ["$$value", [str]]}}},
            },
        },
    ];

    // Test that the default 100MB memory limit isn't reached with our data.
    assert.doesNotThrow(() => coll.aggregate(mapPipeline));
    assert.doesNotThrow(() => coll.aggregate(reducePipeline));

    // Now lower the limit to test that its configuration is obeyed.
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryMaxMapReduceExpressionBytes: memLimitArray}));
    assert.throwsWithCode(() => coll.aggregate(mapPipeline), ErrorCodes.ExceededMemoryLimit);
    assert.throwsWithCode(() => coll.aggregate(reducePipeline), ErrorCodes.ExceededMemoryLimit);
})();

(function testInternalQueryMaxRangeBytesSetting() {
    // Test that the default 100MB memory limit isn't reached with our data.
    assert.doesNotThrow(() => coll.aggregate([{$project: {a: {$range: [0, 100]}}}]));

    // Now lower the limit to test that its configuration is obeyed.
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryMaxRangeBytes: memLimitArray}));
    assert.throwsWithCode(() => coll.aggregate([{$project: {a: {$range: [0, 100]}}}]), ErrorCodes.ExceededMemoryLimit);
})();

MongoRunner.stopMongod(conn);
