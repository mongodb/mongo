/**
 * Collection of helper functions for testing memory tracking statistics in the slow query log,
 * system.profile, and explain("executionStats").
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {iterateMatchingLogLines} from "jstests/libs/log.js";

/******************************************************************************************************
 * Constants for the regexes used to extract memory tracking metrics from the slow query log.
 *******************************************************************************************************/

const maxUsedMemBytesRegex = /maxUsedMemBytes"?:([0-9]+)/;
const inUseMemBytesRegex = /inUseMemBytes"?:([0-9]+)/;
const cursorIdRegex = /cursorid"?:([0-9]+)/;

/******************************************************************************************************
 * Utility functions to extract and detect memory metrics from diagnostic channels.
 ******************************************************************************************************/

function getMetricFromLog(logLine, regex) {
    const match = logLine.match(regex);
    assert(match, `Pattern ${regex} did not match log line: ${logLine}`);
    return parseInt(match[1]);
}

function assertNoMatchInLog(logLine, regex) {
    const match = logLine.match(regex);
    assert.eq(null, match, `Unexpected match for ${regex} in ${logLine}`);
}

function findStageInExplain(explainRes, stageName) {
    for (let i = 0; i < explainRes.stages.length; i++) {
        if (explainRes.stages[i].hasOwnProperty("$" + stageName)) {
            return explainRes.stages[i];
        }
    }
    return null;
}

/**
 * Runs the aggregation and fetches the corresponding diagnostics from the source -- the slow query
 * log or profiler. A pipeline comment is used to identify the log lines or profiler entries
 * corresponding to this aggregation.
 */
function runPipelineAndGetDiagnostics(db, collName, pipeline, pipelineComment, source) {
    // Use toArray() to exhaust the cursor. We use a batchSize of 1 to ensure that a getMore is
    // issued and disallow spilling to disk to prevent clearing of inUseMemBytes.
    db[collName]
        .aggregate(pipeline,
                   {cursor: {batchSize: 1}, comment: pipelineComment, allowDiskUse: false})
        .toArray();

    if (source === "log") {
        let logLines = [];
        assert.soon(() => {
            const globalLog = assert.commandWorked(db.adminCommand({getLog: "global"}));
            logLines = [...iterateMatchingLogLines(globalLog.log,
                                                   {msg: "Slow query", comment: pipelineComment})];
            return logLines.length >= 1;
        }, "Failed to find a log line for comment: " + pipelineComment);
        return logLines;
    } else if (source === "profiler") {
        let profilerEntries = [];
        assert.soon(() => {
            profilerEntries =
                db.system.profile.find({"command.comment": pipelineComment}).toArray();
            return profilerEntries.length >= 1;
        }, "Failed to find a profiler entry for comment: " + pipelineComment);
        return profilerEntries;
    }
}

/******************************************************************************************************
 * Helpers to verify that memory tracking stats are correctly reported for each diagnostic source.
 ******************************************************************************************************/

function verifySlowQueryLogMetrics(
    logLines, expectedNumGetMores, featureFlagEnabled, checkInUseMemBytesResets) {
    if (!featureFlagEnabled) {
        jsTestLog(
            "Test that memory usage metrics do not appear in the slow query logs when the feature flag is off.");

        logLines.forEach(line => assertNoMatchInLog(line, maxUsedMemBytesRegex));
        logLines.forEach(line => assertNoMatchInLog(line, inUseMemBytesRegex));
        return;
    }

    jsTestLog(
        "Test that memory usage metrics appear in the slow query logs and persists across getMore calls when the feature flag is on.");

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

    // Assert that we have one initial request and the expected number of getMores.
    assert.gte(logLines.length,
               expectedNumGetMores + 1,
               `Expected at least ${expectedNumGetMores + 1} log lines ` + tojson(logLines));
    const initialRequests = logLines.filter(line => line.includes('"command":{"aggregate"'));
    const getMores = logLines.filter(line => line.includes('"command":{"getMore"'));
    assert.eq(
        initialRequests.length, 1, "Expected exactly one initial request: " + tojson(logLines));
    assert.gte(getMores.length,
               expectedNumGetMores,
               `Expected at least ${expectedNumGetMores} getMore requests: ` + tojson(logLines));

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
    const exhaustedLines = logLines.filter(line => line.includes('"cursorExhausted"'));
    assert(exhaustedLines.length == 1,
           "Expected to find one log line with cursorExhausted: true: " + tojson(logLines));
    if (checkInUseMemBytesResets) {
        assert(
            !exhaustedLines[0].includes("inUseMemBytes"),
            "inUseMemBytes should not be present in the final getMore since the cursor is exhausted " +
                tojson(exhaustedLines));
    }
}

function verifyProfilerMetrics(
    profilerEntries, expectedNumGetMores, featureFlagEnabled, checkInUseMemBytesResets) {
    if (!featureFlagEnabled) {
        jsTestLog(
            "Test that memory metrics do not appear in the profiler when the feature flag is off.");

        for (const entry of profilerEntries) {
            assert(!entry.hasOwnProperty("maxUsedMemBytes"),
                   "Unexpected maxUsedMemBytes in profiler: " + tojson(entry));
            assert(!entry.hasOwnProperty("inUseMemBytes"),
                   "Unexpected inUseMemBytes in profiler: " + tojson(entry));
        }
        return;
    }
    jsTestLog(
        "Test that memory usage metrics appear in the profiler and persists across getMores when the feature flag is on.");

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
               expectedNumGetMores + 1,
               "Expected at least " + (expectedNumGetMores + 1) + " profiler entries " +
                   tojson(profilerEntries));
    const aggregateEntries = profilerEntries.filter(entry => entry.op === "command");
    const getMoreEntries = profilerEntries.filter(entry => entry.op === "getmore");
    assert.eq(1,
              aggregateEntries.length,
              "Expected exactly one aggregate entry: " + tojson(profilerEntries));
    assert.gte(expectedNumGetMores,
               getMoreEntries.length,
               "Expected at least " + expectedNumGetMores +
                   " getMore entries: " + tojson(profilerEntries));

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
    if (checkInUseMemBytesResets) {
        assert(
            !exhaustedEntry.hasOwnProperty("inUseMemBytes"),
            "inUseMemBytes should not be present in the final getMore since the cursor is exhausted:" +
                tojson(exhaustedEntry));
    }
}

function verifyExplainMetrics(db, collName, pipeline, stageName, featureFlagEnabled) {
    const stageKey = '$' + stageName;
    const explainRes = db[collName].explain("executionStats").aggregate(pipeline);
    let stage;

    if (!featureFlagEnabled) {
        jsTestLog(
            "Test that memory metrics do not appear in the explain output when the feature flag is off.");
        assert(!explainRes.hasOwnProperty("maxUsedMemBytes"),
               "Unexpected maxUsedMemBytes in explain: " + tojson(explainRes));

        // Memory usage metrics do not appear in the stage's statistics. Verify that the stage
        // exists in the explain output.
        stage = findStageInExplain(explainRes, stageName);
        assert(stage, `Expected to find ${stageKey} stage in explain: ` + tojson(explainRes));
        assert(!stage.hasOwnProperty("maxUsedMemBytes"),
               `Unexpected maxUsedMemBytes in ${stageKey} stage: ` + tojson(explainRes));
        return;
    }

    jsTestLog(
        "Test that memory usage metrics appear in the explain output when the feature flag is on.");

    // Memory usage metrics appear in the top-level explain.
    assert(explainRes.hasOwnProperty("maxUsedMemBytes"),
           "Expected maxUsedMemBytes in explain: " + tojson(explainRes));
    assert.gt(explainRes.maxUsedMemBytes,
              0,
              "Expected maxUsedMemBytes to be positive: " + tojson(explainRes));

    // Memory usage metrics appear within the stage's statistics.
    stage = findStageInExplain(explainRes, stageName);
    assert(stage, `Expected to find ${stageKey} stage in explain: ` + tojson(explainRes));
    assert(stage.hasOwnProperty("maxUsedMemBytes"),
           `Unexpected maxUsedMemBytes in ${stageKey} stage: ` + tojson(explainRes));

    jsTestLog(
        "Test that memory usage metrics do not appear in the explain output when the verbosity is lower than executionStats.");
    const explainQueryPlannerRes = db[collName].explain("queryPlanner").aggregate(pipeline);
    assert(!explainQueryPlannerRes.hasOwnProperty("maxUsedMemBytes"),
           "Unexpected maxUsedMemBytes in explain: " + tojson(explainQueryPlannerRes));
    stage = findStageInExplain(explainQueryPlannerRes, stageName);
    assert(stage,
           `Expected to find ${stageKey} stage in explain: ` + tojson(explainQueryPlannerRes));
    assert(!stage.hasOwnProperty("maxUsedMemBytes"),
           `Unexpected maxUsedMemBytes in ${stageKey} stage: ` + tojson(explainQueryPlannerRes));
}

/**
 * For a given pipeline, verify that memory tracking statistics are correctly reported to
 * the slow query log, system.profile, and explain("executionStats").
 */
export function runMemoryStatsTest(
    db,
    collName,
    pipeline,
    pipelineComment,
    stageName,
    expectedNumGetMores,
    checkInUseMemBytesResets = true) {  // TODO SERVER-105637 Remove this param.
    const featureFlagEnabled = FeatureFlagUtil.isPresentAndEnabled(db, "QueryMemoryTracking");
    jsTestLog("QueryMemoryTracking feature flag is " + featureFlagEnabled);

    // Log every operation.
    db.setProfilingLevel(2, {slowms: -1});

    const logLines = runPipelineAndGetDiagnostics(db, collName, pipeline, pipelineComment, "log");

    verifySlowQueryLogMetrics(
        logLines, expectedNumGetMores, featureFlagEnabled, checkInUseMemBytesResets);

    const profilerEntries =
        runPipelineAndGetDiagnostics(db, collName, pipeline, pipelineComment, "profiler");
    verifyProfilerMetrics(
        profilerEntries, expectedNumGetMores, featureFlagEnabled, checkInUseMemBytesResets);

    verifyExplainMetrics(db, collName, pipeline, stageName, featureFlagEnabled);
}
