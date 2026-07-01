// Tests that certain aggregation operators have configurable memory limits.
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB("test");
const coll = db.agg_configurable_memory_limit;

// The explicit plan cache clears below are only needed when SBE is fully enabled; under the classic
// engine the test keeps its original behavior so it still exercises the classic plan cache.
const sbeFullyEnabled = checkSbeFullyEnabled(db);

// Function to change the parameter value and return the previous value.
function setParam(param, val) {
    const res = db.adminCommand({setParameter: 1, [param]: val});
    assert.commandWorked(res);
    // TODO SERVER-67035: Remove this explicit plan cache clear once 'featureFlagSbeFull' is removed.
    // Under SBE full, changing a query knob no longer implicitly clears the SBE plan cache, so clear
    // it explicitly so the new limit is applied rather than reusing a cached plan built with the old
    // limit. Gated on SBE full so we don't mask classic plan cache behavior.
    if (sbeFullyEnabled) {
        coll.getPlanCache().clear();
    }
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
    const res = assert.commandWorked(
        db.adminCommand({getParameter: 1, internalQueryTopNAccumulatorBytes: 1}),
    );
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
        // TODO SERVER-67035: Remove this explicit plan cache clear once 'featureFlagSbeFull' is removed.
        // Under SBE full, changing the knob no longer implicitly clears the SBE plan cache; clear it
        // so the lowered limit is applied rather than reusing the cached plan from the run above.
        // Gated on SBE full so we don't mask classic plan cache behavior.
        if (sbeFullyEnabled) {
            coll.getPlanCache().clear();
        }
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
                a: {
                    $reduce: {
                        input: {$range: [0, 100]},
                        initialValue: [],
                        in: {$concatArrays: ["$$value", [str]]},
                    },
                },
            },
        },
    ];

    // Test that the default 100MB memory limit isn't reached with our data.
    assert.doesNotThrow(() => coll.aggregate(mapPipeline));
    assert.doesNotThrow(() => coll.aggregate(reducePipeline));

    // Now lower the limit to test that its configuration is obeyed.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryMaxMapReduceExpressionBytes: memLimitArray}),
    );
    assert.throwsWithCode(() => coll.aggregate(mapPipeline), ErrorCodes.ExceededMemoryLimit);
    assert.throwsWithCode(() => coll.aggregate(reducePipeline), ErrorCodes.ExceededMemoryLimit);
})();

(function testInternalQueryMaxRangeBytesSetting() {
    // Test that the default 100MB memory limit isn't reached with our data.
    assert.doesNotThrow(() => coll.aggregate([{$project: {a: {$range: [0, 100]}}}]));

    // Now lower the limit to test that its configuration is obeyed.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryMaxRangeBytes: memLimitArray}),
    );
    assert.throwsWithCode(
        () => coll.aggregate([{$project: {a: {$range: [0, 100]}}}]),
        ErrorCodes.ExceededMemoryLimit,
    );
})();

(function testConcatArraysExpressionMemoryLimit() {
    // featureFlagExpressionMemoryTracking turns on the expression evaluation tracking.
    if (!FeatureFlagUtil.isEnabled(db, "ExpressionMemoryTracking")) {
        return;
    }

    // TODO SERVER-129929: remove once SBE tracks $concatArrays.
    if (sbeFullyEnabled) {
        jsTest.log.info(
            "Skipping test because $concatArrays memory tracking is not yet implemented in SBE",
        );
        return;
    }
    const knob = "internalQueryMaxMemoryUsageBytesPerOperation";
    const chunkSizeKnob = "internalQueryMaxWriteToCurOpMemoryUsageBytes";

    const pipeline = [
        {$limit: 1},
        {
            $project: {
                a: {
                    $concatArrays: [
                        {$range: [0, {$add: ["$_id", 50]}]},
                        {$range: [{$add: ["$_id", 50]}, {$add: ["$_id", 100]}]},
                    ],
                },
            },
        },
    ];

    // Unbounded by default (knob at its max). The operation succeeds.
    assert.doesNotThrow(() => coll.aggregate(pipeline).toArray());

    // Lower the operation-wide limit so the $concatArrays result exceeds it.
    const originalVal = setParam(knob, 1024);
    // TODO SERVER-129201: Remove this explicit knob set.
    const chunkOriginalVal = setParam(chunkSizeKnob, 256);

    const err = assert.throwsWithCode(
        () => coll.aggregate(pipeline).toArray(),
        ErrorCodes.ExceededMemoryLimit,
    );
    assert(
        err.message.includes("$concatArrays needs too much memory"),
        "Expected error message to mention $concatArrays, but got: " + err.message,
        {err},
    );

    setParam(knob, originalVal);
    setParam(chunkSizeKnob, chunkOriginalVal);
})();

(function testSetUnionExpressionMemoryLimit() {
    // featureFlagExpressionMemoryTracking turns on the expression evaluation tracking.
    if (!FeatureFlagUtil.isEnabled(db, "ExpressionMemoryTracking")) {
        jsTest.log.info("Skipping test because ExpressionMemoryTracking is not enabled");
        return;
    }
    // TODO SERVER-129889: remove once SBE tracks $setUnion.
    if (sbeFullyEnabled) {
        jsTest.log.info(
            "Skipping test because $setUnion memory tracking is not yet implemented in SBE",
        );
        return;
    }
    const knob = "internalQueryMaxMemoryUsageBytesPerOperation";
    const chunkSizeKnob = "internalQueryMaxWriteToCurOpMemoryUsageBytes";

    const pipeline = [
        {$limit: 1},
        {
            $project: {
                a: {
                    $setUnion: [
                        {$range: [0, {$add: ["$_id", 50]}]},
                        {$range: [{$add: ["$_id", 50]}, {$add: ["$_id", 100]}]},
                    ],
                },
            },
        },
    ];

    // The operation succeeds with the default limit.
    assert.doesNotThrow(() => coll.aggregate(pipeline).toArray());

    // Lower the operation-wide limit so the $setUnion result exceeds it.
    const originalVal = setParam(knob, 1024);
    // TODO SERVER-129201: Remove this explicit knob set.
    const chunkOriginalVal = setParam(chunkSizeKnob, 256);

    const err = assert.throwsWithCode(
        () => coll.aggregate(pipeline).toArray(),
        ErrorCodes.ExceededMemoryLimit,
    );
    assert(
        err.message.includes("$setUnion needs too much memory"),
        "Expected error message to mention $setUnion, but got: " + err.message,
        {err},
    );

    setParam(knob, originalVal);
    setParam(chunkSizeKnob, chunkOriginalVal);
})();

(function testConvertExpressionMemoryLimit() {
    // featureFlagExpressionMemoryTracking turns on the expression evaluation tracking.
    if (!FeatureFlagUtil.isEnabled(db, "ExpressionMemoryTracking")) {
        return;
    }

    const knob = "internalQueryMaxMemoryUsageBytesPerOperation";
    const chunkSizeKnob = "internalQueryMaxWriteToCurOpMemoryUsageBytes";

    // BinData -> array has by far the largest blow-up of any $convert conversion (one array element
    // per vector entry, so the output can be many times the input size), so it is the only tracked
    // case. It requires the vector conversion feature flag.
    if (!FeatureFlagUtil.isEnabled(db, "ConvertBinDataVectors")) {
        return;
    }

    // Build a vector-subtype BinData from an array field, then convert it back to an array.
    const memColl = db.agg_convert_memory_limit;
    memColl.drop();
    const bigArray = Array.from({length: 2000}, (_, i) => i % 100);
    assert.commandWorked(memColl.insert({_id: 0, arr: bigArray}));
    const pipeline = [
        {$limit: 1},
        {
            $project: {
                a: {
                    $convert: {
                        input: {
                            $convert: {
                                input: "$arr",
                                to: {type: "binData", subtype: 9},
                                format: "base64",
                            },
                        },
                        to: "array",
                        format: "base64",
                    },
                },
            },
        },
    ];

    assert.doesNotThrow(() => memColl.aggregate(pipeline).toArray());

    const originalVal = setParam(knob, 1024);
    // TODO SERVER-129201: Remove this explicit knob set.
    const chunkOriginalVal = setParam(chunkSizeKnob, 256);

    const err = assert.throwsWithCode(
        () => memColl.aggregate(pipeline).toArray(),
        ErrorCodes.ExceededMemoryLimit,
    );
    assert(
        err.message.includes("$convert needs too much memory"),
        "Expected error message to mention $convert, but got: " + err.message,
        {err},
    );

    setParam(knob, originalVal);
    setParam(chunkSizeKnob, chunkOriginalVal);
})();

(function testConcatExpressionMemoryLimit() {
    // featureFlagExpressionMemoryTracking turns on the expression evaluation tracking.
    if (!FeatureFlagUtil.isEnabled(db, "ExpressionMemoryTracking")) {
        jsTest.log.info("Skipping test because ExpressionMemoryTracking is not enabled");
        return;
    }

    // TODO SERVER-129995: remove once SBE tracks $concat.
    if (sbeFullyEnabled) {
        jsTest.log.info(
            "Skipping test because $concat memory tracking is not yet implemented in SBE",
        );
        return;
    }
    const knob = "internalQueryMaxMemoryUsageBytesPerOperation";
    const chunkSizeKnob = "internalQueryMaxWriteToCurOpMemoryUsageBytes";

    const big = "x".repeat(2000);
    const pipeline = [{$limit: 1}, {$project: {a: {$concat: ["$y", big]}}}];

    assert.doesNotThrow(() => coll.aggregate(pipeline).toArray());

    const originalVal = setParam(knob, 1024);
    // TODO SERVER-129201: Remove this explicit knob set.
    const chunkOriginalVal = setParam(chunkSizeKnob, 256);

    const err = assert.throwsWithCode(
        () => coll.aggregate(pipeline).toArray(),
        ErrorCodes.ExceededMemoryLimit,
    );
    assert(
        err.message.includes("$concat needs too much memory"),
        "Expected error message to mention $concat, but got: " + err.message,
        {err},
    );

    setParam(knob, originalVal);
    setParam(chunkSizeKnob, chunkOriginalVal);
})();

(function testArrayExpressionMemoryLimit() {
    // featureFlagExpressionMemoryTracking turns on the expression evaluation tracking.
    if (!FeatureFlagUtil.isEnabled(db, "ExpressionMemoryTracking")) {
        jsTest.log.info("Skipping test because ExpressionMemoryTracking is not enabled");
        return;
    }

    // TODO SERVER-130093: remove once SBE tracks $array (ExpressionArray).
    if (sbeFullyEnabled) {
        jsTest.log.info(
            "Skipping test because $array memory tracking is not yet implemented in SBE",
        );
        return;
    }
    const knob = "internalQueryMaxMemoryUsageBytesPerOperation";
    const chunkSizeKnob = "internalQueryMaxWriteToCurOpMemoryUsageBytes";

    const pipeline = [
        {$limit: 1},
        {
            $project: {
                a: [
                    {$range: [0, {$add: ["$_id", 50]}]},
                    {$range: [{$add: ["$_id", 50]}, {$add: ["$_id", 100]}]},
                ],
            },
        },
    ];

    assert.doesNotThrow(() => coll.aggregate(pipeline).toArray());

    // Lower the operation-wide limit so the $array result exceeds it.
    const originalVal = setParam(knob, 1024);
    // TODO SERVER-129201: Remove this explicit knob set.
    const chunkOriginalVal = setParam(chunkSizeKnob, 256);

    const err = assert.throwsWithCode(
        () => coll.aggregate(pipeline).toArray(),
        ErrorCodes.ExceededMemoryLimit,
    );
    assert(
        err.message.includes("$array needs too much memory"),
        "Expected error message to mention $array, but got: " + err.message,
        {err},
    );

    setParam(knob, originalVal);
    setParam(chunkSizeKnob, chunkOriginalVal);
})();

MongoRunner.stopMongod(conn);
