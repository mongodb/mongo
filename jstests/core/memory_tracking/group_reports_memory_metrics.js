/**
 * Tests that, when the memory tracking feature flag is enabled, memory tracking statistics are
 * reported to the slow query log and system.profile for aggregations with $group using the classic
 * engine.
 * @tags: [
 * requires_profiling,
 * requires_getmore,
 * # The test queries the system.profile collection so it is not compatible with initial sync
 * # since an initial sync may insert unexpected operations into the profile collection.
 * queries_system_profile_collection,
 * # The test runs the profile and getLog commands, which are not supported in Serverless.
 * command_not_supported_in_serverless,
 * ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {iterateMatchingLogLines} from "jstests/libs/log.js";

db.coll.drop();

// Get the current value of the query framework server parameter so we can restore it at the end of
// the test. Otherwise, the tests run after this will be affected.
const kOriginalInternalQueryFrameworkControl =
    assert.commandWorked(db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1}))
        .internalQueryFrameworkControl;
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}));

// Setup test collection.
assert.commandWorked(db.coll.insertMany([
    {groupKey: 1, val: "a"},
    {groupKey: 1, val: "b"},
    {groupKey: 2, val: "c"},
    {groupKey: 2, val: "d"},
]));

// Log every operation.
db.setProfilingLevel(2, {slowms: -1});

/** SLOW QUERY LOG TESTS **/

// TODO SERVER-103013 Move this into a utils file.
const maxUsedMemBytesRegex = /maxUsedMemBytes"?:([0-9]+)/;
const inUseMemBytesRegex = /inUseMemBytes"?:([0-9]+)/;
const cursorIdRegex = /cursorid"?:([0-9]+)/;

function getMetricFromLog(logLine, regex) {
    const match = logLine.match(regex);
    assert(match, `Pattern ${regex} did not match log line: ${logLine}`);
    return parseInt(match[1]);
}

function assertNoMatchInLog(logLine, regex) {
    const match = logLine.match(regex);
    assert.eq(null, match, `Unexpected match for ${regex} in ${logLine}`);
}

// TODO SERVER-103013 Move this into a utils file.
// Runs the aggregation and fetches the corresponding slow query log lines.
function runPipelineAndGetSlowQueryLogLines(pipeline, pipelineComment) {
    // Use toArray() to exhaust the cursor. We use a batchSize of 1 to ensure that a getMore is
    // issued and disallow spilling to disk to prevent clearing of inUseMemBytes.
    db.coll
        .aggregate(pipeline,
                   {cursor: {batchSize: 1}, comment: pipelineComment, allowDiskUse: false})
        .toArray();

    const globalLog = assert.commandWorked(db.adminCommand({getLog: "global"}));
    const logLines =
        [...iterateMatchingLogLines(globalLog.log, {msg: "Slow query", comment: pipelineComment})];
    assert(logLines.length >= 1, "Failed to find a log line for comment: " + pipelineComment);
    return logLines;
}

if (!FeatureFlagUtil.isPresentAndEnabled(db, "QueryMemoryTracking")) {
    jsTestLog(
        "Test that memory usage metrics do not appear in the slow query logs when the feature flag is off.");
    const logLines =
        runPipelineAndGetSlowQueryLogLines([{$group: {_id: "$groupKey", values: {$push: "$val"}}}],
                                           "log test: group without memory tracking");

    logLines.forEach(line => assertNoMatchInLog(line, maxUsedMemBytesRegex));
    logLines.forEach(line => assertNoMatchInLog(line, inUseMemBytesRegex));

} else {
    jsTestLog(
        "Test that memory usage metrics appear in the slow query logs and persists across getMore calls when the feature flag is on.");
    let logLines =
        runPipelineAndGetSlowQueryLogLines([{$group: {_id: "$groupKey", values: {$push: "$val"}}}],
                                           "log test: group memory tracking across getMores");

    // Get the cursorId from the initial request. We filter based off of the original cursorId
    // because some passthrough configs will re-issue queries.
    //
    // Because of asynchronous log flushing, the first log line extracted may not correspond to the
    // original request.
    const originalRequestLogLine = logLines.find(line => line.includes('"command":{"aggregate"'));
    const cursorId = getMetricFromLog(originalRequestLogLine, cursorIdRegex);
    logLines = logLines.filter(line => {
        return getMetricFromLog(line, cursorIdRegex).valueOf() === cursorId.valueOf();
    });

    // Assert that we have one aggregate and two getMores.
    assert.gte(logLines.length, 3, "Expected at least 3 log lines " + tojson(logLines));
    const initialRequests = logLines.filter(line => line.includes('"command":{"aggregate"'));
    const getMores = logLines.filter(line => line.includes('"command":{"getMore"'));
    assert.eq(
        initialRequests.length, 1, "Expected exactly one initial request: " + tojson(logLines));
    assert.gte(getMores.length, 2, "Expected at least two getMore requests: " + tojson(logLines));

    // Check that inUseMemBytes is non-zero when the cursor is still in-use.
    let peakInUseMem = 0;
    for (const line of logLines) {
        if (!line.includes('"cursorExhausted"')) {
            const inUseMemBytes = getMetricFromLog(line, inUseMemBytesRegex);
            peakInUseMem = Math.max(inUseMemBytes, peakInUseMem);
        }

        const maxUsedMemBytes = getMetricFromLog(line, maxUsedMemBytesRegex);
        assert.gte(maxUsedMemBytes,
                   peakInUseMem,
                   `maxUsedMemBytes (${maxUsedMemBytes}) should be >= peak inUseMemBytes (${
                       peakInUseMem}) seen so far\n` +
                       tojson(logLines));
    }

    // The cursor is exhausted and the pipeline's resources have been freed, so the last
    // inUseMemBytes should be 0.
    const exhaustedLine = logLines.filter(line => line.includes('"cursorExhausted"'));
    assert(exhaustedLine,
           "Expected to find a log line with cursorExhausted: true: " + tojson(logLines));
    assert(
        !exhaustedLine.includes("inUseMemBytes"),
        "inUseMemBytes should not be present in the final getMore since the cursor is exhausted " +
            tojson(exhaustedLine));
}

/** PROFILER TESTS **/

// TODO SERVER-103013 Move this into a utils file.
// Function to run aggregation and collect multiple profiler entries.
function runPipelineAndGetProfilerEntries(pipeline, comment) {
    db.coll.aggregate(pipeline, {cursor: {batchSize: 1}, comment, allowDiskUse: false}).toArray();

    return db.system.profile.find({"command.comment": comment}).toArray();
}

if (!FeatureFlagUtil.isPresentAndEnabled(db, "QueryMemoryTracking")) {
    jsTestLog(
        "Test that memory metrics do not appear in the profiler when the feature flag is off.");
    const profilerEntries =
        runPipelineAndGetProfilerEntries([{$group: {_id: "$groupKey", values: {$push: "$val"}}}],
                                         "profiler test: group without memory tracking");

    for (const entry of profilerEntries) {
        assert(!entry.hasOwnProperty("maxUsedMemBytes"),
               "Unexpected maxUsedMemBytes in profiler: " + tojson(entry));
        assert(!entry.hasOwnProperty("inUseMemBytes"),
               "Unexpected inUseMemBytes in profiler: " + tojson(entry));
    }
} else {
    jsTestLog(
        "Test that memory usage metrics appear in the profiler and persists across getMores when the feature flag is on.");
    let profilerEntries =
        runPipelineAndGetProfilerEntries([{$group: {_id: "$groupKey", values: {$push: "$val"}}}],
                                         "profiler test: group with memory tracking");

    // Get the cursorId from the initial request. We filter based off of the original cursorId
    // because we only want to check memory usages corresponding to this test. Otherwise, some
    // passthrough configs will re-issue queries, which would affect memory entries returned.
    //
    // Because of async flushing, the original request might not correspond to the first entry, so
    // search for the aggregate entry.
    const originalRequestProfilerEntry = profilerEntries.find(entry => entry.command.aggregate);
    assert(originalRequestProfilerEntry,
           "Could not find original aggregate request in profilerEntries");
    const cursorId = originalRequestProfilerEntry.cursorid;
    profilerEntries = profilerEntries.filter(entry => entry.cursorid &&
                                                 entry.cursorid.valueOf() === cursorId.valueOf());

    // Assert that we have one aggregate and two getMores.
    assert.gte(profilerEntries.length,
               3,
               "Expected at least 3 profiler entries " + tojson(profilerEntries));
    const aggregateEntries = profilerEntries.filter(entry => entry.op === "command");
    const getMoreEntries = profilerEntries.filter(entry => entry.op === "getmore");
    assert.eq(1,
              aggregateEntries.length,
              "Expected exactly one aggregate entry: " + tojson(profilerEntries));
    assert.gte(2,
               getMoreEntries.length,
               "Expected at least two getMore entries: " + tojson(profilerEntries));

    // Check that inUseMemBytes is non-zero when the cursor is still in use.
    let peakInUseMem = 0;
    for (const entry of profilerEntries) {
        if (!entry.cursorExhausted) {
            assert.gt(
                entry.inUseMemBytes,
                0,
                "Expected inUseMemBytes to be nonzero in getMore: " + tojson(profilerEntries));
            peakInUseMem = Math.max(peakInUseMem, entry.inUseMemBytes);
        }

        assert(entry.hasOwnProperty("maxUsedMemBytes"),
               `Missing maxUsedMemBytes in profiler entry: ${tojson(profilerEntries)}`);
        assert.gte(entry.maxUsedMemBytes,
                   peakInUseMem,
                   `maxUsedMemBytes (${entry.maxUsedMemBytes}) should be >= inUseMemBytes peak (${
                       peakInUseMem}) at this point: ` +
                       tojson(entry));
    }

    // No memory is currently in use because the cursor is exhausted.
    const exhaustedEntry = profilerEntries.find(entry => entry.cursorExhausted);
    assert(
        exhaustedEntry,
        "Expected to find a profiler entry with cursorExhausted: true: " + tojson(profilerEntries));
    assert(
        !exhaustedEntry.hasOwnProperty("inUseMemBytes"),
        "inUseMemBytes should not be present in the final getMore since the cursor is exhausted: " +
            tojson(exhaustedEntry));
}

// Clean up.
db.coll.drop();
assert.commandWorked(db.adminCommand(
    {setParameter: 1, internalQueryFrameworkControl: kOriginalInternalQueryFrameworkControl}));
