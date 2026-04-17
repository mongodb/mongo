/**
 * Tests that, when the memory tracking feature flag is enabled, memory tracking statistics are
 * reported to the slow query log, system.profile, and explain("executionStats") for the UPDATE
 * stage.
 *
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   does_not_support_transactions,
 *   requires_fcv_90,
 *   requires_profiling,
 *   # The test queries the system.profile collection so it is not compatible with initial sync
 *   # since an initial sync may insert unexpected operations into the profile collection.
 *   queries_system_profile_collection,
 *   # The test runs the profile and getLog commands, which are not supported in Serverless.
 *   command_not_supported_in_serverless,
 * ]
 */

import {getPlanStage} from "jstests/libs/query/analyze_plan.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {runPipelineAndGetDiagnostics} from "jstests/libs/query/memory_tracking_utils.js";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB("test");

const collName = jsTestName();
const coll = db[collName];
coll.drop();

const kDocCount = 100;
const docs = Array.from({length: kDocCount}, (_, i) => ({_id: i, x: i, y: 0}));
assert.commandWorked(coll.insertMany(docs));
// Index on x. Multi-updates that modify x set indexesAffected=true, inserting record ids into
// the deduplicator.
assert.commandWorked(coll.createIndex({x: 1}));

const updateCommand = {
    update: collName,
    updates: [{q: {x: {$gte: 0}}, u: {$inc: {x: kDocCount}}, multi: true}],
};

const explainExecStats = assert.commandWorked(db.runCommand({explain: updateCommand, verbosity: "executionStats"}));
const updateStage = getPlanStage(explainExecStats.executionStats.executionStages, "UPDATE");

if (updateStage === null) {
    jsTest.log.info(
        "Skipping test: query did not use the classic UPDATE stage. " +
            "This stage is only used by the classic engine.",
    );
    coll.drop();
    MongoRunner.stopMongod(conn);
    quit();
}

const featureFlagEnabled = FeatureFlagUtil.isPresentAndEnabled(db, "QueryMemoryTracking");

//Test that memory usage metrics appear in the explain output when the verbosity is executionStats.
if (featureFlagEnabled) {
    assert(
        updateStage.hasOwnProperty("peakTrackedMemBytes"),
        "Expected peakTrackedMemBytes in UPDATE stage: " + tojson(explainExecStats),
    );
    assert.gt(updateStage.peakTrackedMemBytes, 0, "Expected positive peakTrackedMemBytes: " + tojson(explainExecStats));
} else {
    assert(
        !updateStage.hasOwnProperty("peakTrackedMemBytes"),
        "Unexpected peakTrackedMemBytes: " + tojson(explainExecStats),
    );
}

// Test that memory usage metrics do not appear in the explain output when the verbosity is lower than executionStats.
const explainQueryPlannerRes = assert.commandWorked(db.runCommand({explain: updateCommand, verbosity: "queryPlanner"}));
assert(
    !explainQueryPlannerRes.hasOwnProperty("peakTrackedMemBytes"),
    "Unexpected peakTrackedMemBytes: " + tojson(explainQueryPlannerRes),
);

const peakTrackedMemBytesRegex = /peakTrackedMemBytes"?:([0-9]+)/;

// Log every operation.
db.setProfilingLevel(2, {slowms: -1});

const updateCommandWithComment = Object.assign({}, updateCommand, {comment: "memory stats update stage test"});

// Collect slow query log entries for the update. An update command produces two slow query log
// entries: one from the COMMAND component (the outer update command) and one from the WRITE
// component (the actual write operation). Only the WRITE entry contains memory metrics.
const logLines = runPipelineAndGetDiagnostics({
    db,
    collName,
    commandObj: updateCommandWithComment,
    source: "log",
}).filter((line) => line.includes('"c":"WRITE"'));

// Collect profiler entries for the update. An update command produces two profiler entries: one
// with op "command" (the outer update command) and one with op "update" (the actual write
// operation). Only the "update" entry contains memory metrics.
const profilerEntries = runPipelineAndGetDiagnostics({
    db,
    collName,
    commandObj: updateCommandWithComment,
    source: "profiler",
}).filter((entry) => entry.op === "update");

// --- Slow query log check ---
// Test that memory usage metrics appear in the slow query log.
assert.eq(1, logLines.length, "Expected exactly one slow query log entry: " + tojson(logLines));
if (featureFlagEnabled) {
    const match = logLines[0].match(peakTrackedMemBytesRegex);
    assert(match, "Expected peakTrackedMemBytes in slow query log: " + logLines[0]);
    assert.gt(parseInt(match[1]), 0, "Expected positive peakTrackedMemBytes in slow query log: " + logLines[0]);
} else {
    assert(
        !peakTrackedMemBytesRegex.test(logLines[0]),
        "Unexpected peakTrackedMemBytes in slow query log: " + logLines[0],
    );
}

// --- Profiler check ---
// Test that memory usage metrics appear in the profiler.
assert.eq(1, profilerEntries.length, "Expected exactly one profiler entry: " + tojson(profilerEntries));
const profilerEntry = profilerEntries[0];
if (featureFlagEnabled) {
    assert(
        profilerEntry.hasOwnProperty("peakTrackedMemBytes"),
        "Expected peakTrackedMemBytes in profiler: " + tojson(profilerEntry),
    );
    assert.gt(
        profilerEntry.peakTrackedMemBytes,
        0,
        "Expected positive peakTrackedMemBytes in profiler: " + tojson(profilerEntry),
    );
} else {
    assert(
        !profilerEntry.hasOwnProperty("peakTrackedMemBytes"),
        "Unexpected peakTrackedMemBytes in profiler: " + tojson(profilerEntry),
    );
}

// Clean up.
coll.drop();
MongoRunner.stopMongod(conn);
