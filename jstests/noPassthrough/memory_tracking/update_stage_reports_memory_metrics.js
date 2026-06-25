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
import {runPipelineAndGetDiagnostics} from "jstests/libs/query/memory_tracking_utils.js";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB("test");
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryMaxWriteToServerStatusMemoryUsageBytes: 1}),
);
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}),
);

const collName = jsTestName();
const coll = db[collName];
coll.drop();

const kDocCount = 100;
const docs = Array.from({length: kDocCount}, (_, i) => ({_id: i, x: i, y: 0}));
assert.commandWorked(coll.insertMany(docs));
// Index on x. Multi-updates that modify x set indexesAffected=true, inserting record ids into
// the deduplicator.
assert.commandWorked(coll.createIndex({x: 1}));

const updateEntry = {q: {x: {$gte: 0}}, u: {$inc: {x: kDocCount}}};
const updateCommand = {update: collName, updates: [updateEntry]};
const multiUpdateCommand = {...updateCommand, updates: [{...updateEntry, multi: true}]};

// Explain populates the per-operation deduplicator (so peakTrackedMemBytes is reported) but must
// not increment the server-wide deduplication counters. Snapshot them before the explain run.
const dedupBeforeExplain = db.serverStatus().metrics.query.recordIdDeduplication.UPDATE;

const explainExecStats = assert.commandWorked(
    db.runCommand({explain: multiUpdateCommand, verbosity: "executionStats"}),
);
const updateStage = getPlanStage(explainExecStats.executionStats.executionStages, "UPDATE");

assert.neq(
    null,
    updateStage,
    "Expected query to use the classic UPDATE stage with forceClassicEngine ",
);

// In explain mode the update is not actually written, but the records that would have been updated
// are tracked in the deduplicator, so peakTrackedMemBytes reflects that memory usage.
assert(
    updateStage.hasOwnProperty("peakTrackedMemBytes"),
    "Expected peakTrackedMemBytes in UPDATE explain",
    {
        explainExecStats,
    },
);
assert.gt(
    updateStage.peakTrackedMemBytes,
    0,
    "Expected positive peakTrackedMemBytes in UPDATE explain",
    {
        explainExecStats,
    },
);

// The explain run must not have touched the global deduplication serverStatus counters.
const dedupAfterExplain = db.serverStatus().metrics.query.recordIdDeduplication.UPDATE;
assert.eq(
    dedupAfterExplain.deduplicatedBytes,
    dedupBeforeExplain.deduplicatedBytes,
    "Explain should not increment global deduplicatedBytes",
);
assert.eq(
    dedupAfterExplain.deduplicatedRecords,
    dedupBeforeExplain.deduplicatedRecords,
    "Explain should not increment global deduplicatedRecords",
);

const peakTrackedMemBytesRegex = /peakTrackedMemBytes"?:([0-9]+)/;

// Log every operation.
db.setProfilingLevel(2, {slowms: -1});

const updateCommandWithComment = {...multiUpdateCommand, comment: "memory stats update stage test"};

// Collect slow query log entries for the update. An update command produces two slow query log
// entries: one from the COMMAND component (the outer update command) and one from the WRITE
// component (the actual write operation). Only the WRITE entry contains memory metrics.
const logLines = runPipelineAndGetDiagnostics({
    db,
    collName,
    commandObj: updateCommandWithComment,
    source: "log",
}).filter((line) => line.includes('"c":"WRITE"'));

const dedupBefore = db.serverStatus().metrics.query.recordIdDeduplication.UPDATE;

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
const match = logLines[0].match(peakTrackedMemBytesRegex);
assert(match, "Expected peakTrackedMemBytes in slow query log: " + logLines[0]);
assert.gt(
    parseInt(match[1]),
    0,
    "Expected positive peakTrackedMemBytes in slow query log: " + logLines[0],
);

// --- Profiler check ---
// Test that memory usage metrics appear in the profiler.
assert.eq(
    1,
    profilerEntries.length,
    "Expected exactly one profiler entry: " + tojson(profilerEntries),
);
const profilerEntry = profilerEntries[0];
assert(
    profilerEntry.hasOwnProperty("peakTrackedMemBytes"),
    "Expected peakTrackedMemBytes in profiler: " + tojson(profilerEntry),
);
assert.gt(
    profilerEntry.peakTrackedMemBytes,
    0,
    "Expected positive peakTrackedMemBytes in profiler: " + tojson(profilerEntry),
);

// The explain run tracked the same set of record IDs as the real write, so their memory
// footprints should match.
assert.eq(
    updateStage.peakTrackedMemBytes,
    profilerEntry.peakTrackedMemBytes,
    "Explain should track the same memory as a real write",
    {updateStage, profilerEntry},
);

// Verify DeduplicatorReporter serverStatus metrics for the UPDATE stage.
// dedupBefore was captured after the log pipeline, so the diff reflects only the profiler run,
// which inserts all kDocCount record IDs into a fresh deduplicator.
const updateDedupStats = db.serverStatus().metrics.query.recordIdDeduplication.UPDATE;
assert.gt(
    updateDedupStats.deduplicatedBytes - dedupBefore.deduplicatedBytes,
    0,
    "Expected positive deduplicatedBytes diff for UPDATE stage: " + tojson(updateDedupStats),
);
assert.eq(
    updateDedupStats.deduplicatedRecords - dedupBefore.deduplicatedRecords,
    kDocCount,
    "Expected deduplicatedRecords diff of kDocCount for UPDATE stage: " + tojson(updateDedupStats),
);

// Verify that a non-multi update does not increment the deduplication counters, since
// _updatedRecordIds is only allocated for multi updates.
assert.commandWorked(db.runCommand({...updateCommand, updates: [{...updateEntry, multi: false}]}));
const dedupAfter = db.serverStatus().metrics.query.recordIdDeduplication.UPDATE;
assert.eq(
    dedupAfter.deduplicatedBytes,
    updateDedupStats.deduplicatedBytes,
    "Single update should not increment deduplicatedBytes",
);
assert.eq(
    dedupAfter.deduplicatedRecords,
    updateDedupStats.deduplicatedRecords,
    "Single update should not increment deduplicatedRecords",
);

// With secondary indexes but updating a non-indexed field: the diff does not touch any
// indexed field, so the deduplicator should not be populated and peakTrackedMemBytes
// should be absent.
const nonIndexedFieldUpdate = {q: {}, u: {$inc: {y: 1}}};
const nonIndexedFieldCommand = {
    update: collName,
    updates: [{...nonIndexedFieldUpdate, multi: true}],
};
const explainNonIndexed = assert.commandWorked(
    db.runCommand({explain: nonIndexedFieldCommand, verbosity: "executionStats"}),
);
const updateStageNonIndexed = getPlanStage(
    explainNonIndexed.executionStats.executionStages,
    "UPDATE",
);
assert(
    !updateStageNonIndexed.hasOwnProperty("peakTrackedMemBytes"),
    "Expected no peakTrackedMemBytes when update does not touch any indexed field",
    {explainNonIndexed},
);

// Verify that explain respects the memory limit: with the limit set to 1 byte, inserting even
// a single record ID into the deduplicator exceeds it and explain should fail.
const originalMemoryLimit = assert.commandWorked(
    db.adminCommand({getParameter: 1, internalUpdateStageMaxMemoryBytes: 1}),
).internalUpdateStageMaxMemoryBytes;
try {
    assert.commandWorked(db.adminCommand({setParameter: 1, internalUpdateStageMaxMemoryBytes: 1}));
    const explainOOM = assert.commandWorked(
        db.runCommand({explain: multiUpdateCommand, verbosity: "executionStats"}),
    );
    assert.eq(
        explainOOM.executionStats.errorCode,
        12227902,
        "Expected memory limit error in explain executionStats",
        {
            explainOOM,
        },
    );
} finally {
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalUpdateStageMaxMemoryBytes: originalMemoryLimit}),
    );
}

// Without secondary indexes there is no Halloween problem, so the deduplicator is never
// populated and explain should not report peakTrackedMemBytes.
coll.dropIndex("x_1");
const explainNoIndex = assert.commandWorked(
    db.runCommand({explain: multiUpdateCommand, verbosity: "executionStats"}),
);
const updateStageNoIndex = getPlanStage(explainNoIndex.executionStats.executionStages, "UPDATE");
assert(
    !updateStageNoIndex.hasOwnProperty("peakTrackedMemBytes"),
    "Expected no peakTrackedMemBytes for explain on collection with no secondary indexes",
    {explainNoIndex},
);

// Clean up.
coll.drop();
MongoRunner.stopMongod(conn);
