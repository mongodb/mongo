/**
 * Collection of helper functions for testing memory tracking statistics in the slow query log,
 * system.profile, and explain("executionStats").
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {iterateMatchingLogLines} from "jstests/libs/log.js";
import {getAggPlanStages} from "jstests/libs/query/analyze_plan.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

/******************************************************************************************************
 * Constants for the regexes used to extract memory tracking metrics from the slow query log.
 *******************************************************************************************************/

const maxUsedMemBytesRegex = /maxUsedMemBytes"?:([0-9]+)/;
const inUseMemBytesRegex = /inUseMemBytes"?:([0-9]+)/;
const cursorIdRegex = /cursorid"?:([0-9]+)/;

/******************************************************************************************************
 * Utility functions to extract and detect memory metrics from diagnostic channels.
 ******************************************************************************************************/

function getMetricFromLog(logLine, regex, doAssert = true) {
    const match = logLine.match(regex);
    if (doAssert) {
        assert(match, `Pattern ${regex} did not match log line: ${logLine}`);
    } else if (!match) {
        return -1;
    }

    return parseInt(match[1]);
}

function assertNoMatchInLog(logLine, regex) {
    const match = logLine.match(regex);
    assert.eq(null, match, `Unexpected match for ${regex} in ${logLine}`);
}

/**
 * Runs the aggregation and fetches the corresponding diagnostics from the source -- the slow query
 * log or profiler. A pipeline comment is used to identify the log lines or profiler entries
 * corresponding to this aggregation.
 */
function runPipelineAndGetDiagnostics({db, collName, commandObj, source}) {
    const pipelineComment = commandObj.comment;

    // Use toArray() to exhaust the cursor.
    const options = {
        comment: pipelineComment,
        cursor: commandObj.cursor,
        allowDiskUse: commandObj.allowDiskUse,
    };
    db[collName].aggregate(commandObj.pipeline, options).toArray();

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

function verifySlowQueryLogMetrics({
    logLines,
    verifyOptions,
}) {
    if (!verifyOptions.featureFlagEnabled) {
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
    assert(originalRequestLogLine,
           "Failed to find original aggregate request in log lines: " + tojson(logLines));
    const cursorId = getMetricFromLog(originalRequestLogLine, cursorIdRegex);
    logLines = logLines.filter(line => {
        return getMetricFromLog(line, cursorIdRegex).valueOf() === cursorId.valueOf();
    });

    // Assert that we have one initial request and the expected number of getMores.
    assert.gte(
        logLines.length,
        verifyOptions.expectedNumGetMores + 1,
        `Expected at least ${verifyOptions.expectedNumGetMores + 1} log lines ` + tojson(logLines));
    const initialRequests = logLines.filter(line => line.includes('"command":{"aggregate"'));
    const getMores = logLines.filter(line => line.includes('"command":{"getMore"'));
    assert.eq(
        initialRequests.length, 1, "Expected exactly one initial request: " + tojson(logLines));
    assert.gte(getMores.length,
               verifyOptions.expectedNumGetMores,
               `Expected at least ${verifyOptions.expectedNumGetMores} getMore requests: ` +
                   tojson(logLines));

    // Check that inUseMemBytes is non-zero when the cursor is still in-use.
    let peakInUseMem = 0;
    let foundInUseMem = false;
    for (const line of logLines) {
        if (!verifyOptions.skipInUseMemBytesCheck && !line.includes('"cursorExhausted"')) {
            const inUseMemBytes =
                getMetricFromLog(line, inUseMemBytesRegex, false /* don't assert */);
            if (inUseMemBytes > 0) {
                peakInUseMem = Math.max(inUseMemBytes, peakInUseMem);
                foundInUseMem = true;
            }
        }

        const maxUsedMemBytes = getMetricFromLog(line, maxUsedMemBytesRegex);
        assert.gte(maxUsedMemBytes,
                   peakInUseMem,
                   `maxUsedMemBytes (${maxUsedMemBytes}) should be >= peak inUseMemBytes (${
                       peakInUseMem}) seen so far\n` +
                       tojson(logLines));
    }

    if (!verifyOptions.skipInUseMemBytesCheck) {
        assert(foundInUseMem, "Expected to find inUseMemBytes in slow query logs at least once");
    }

    // The cursor is exhausted and the pipeline's resources have been freed, so the last
    // inUseMemBytes should be 0.
    const exhaustedLines = logLines.filter(line => line.includes('"cursorExhausted"'));
    assert(exhaustedLines.length == 1,
           "Expected to find one log line with cursorExhausted: true: " + tojson(logLines));
    if (verifyOptions.checkInUseMemBytesResets && !verifyOptions.skipInUseMemBytesCheck) {
        assert(
            !exhaustedLines[0].includes("inUseMemBytes"),
            "inUseMemBytes should not be present in the final getMore since the cursor is exhausted " +
                tojson(exhaustedLines));
    }
}

function verifyProfilerMetrics({
    profilerEntries,
    verifyOptions,
}) {
    if (!verifyOptions.featureFlagEnabled) {
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

    // Assert that we have one aggregate and 'expectedNumGetMores' getMores.
    assert.gte(profilerEntries.length,
               verifyOptions.expectedNumGetMores + 1,
               "Expected at least " + (verifyOptions.expectedNumGetMores + 1) +
                   " profiler entries " + tojson(profilerEntries));
    const aggregateEntries = profilerEntries.filter(entry => entry.op === "command");
    const getMoreEntries = profilerEntries.filter(entry => entry.op === "getmore");
    assert.eq(1,
              aggregateEntries.length,
              "Expected exactly one aggregate entry: " + tojson(profilerEntries));
    assert.gte(getMoreEntries.length,
               verifyOptions.expectedNumGetMores,
               "Expected at least " + verifyOptions.expectedNumGetMores +
                   " getMore entries: " + tojson(profilerEntries));

    // Check that inUseMemBytes is non-zero when the cursor is still in use.
    let peakInUseMem = 0;
    let foundInUseMem = false;
    for (const entry of profilerEntries) {
        if (!verifyOptions.skipInUseMemBytesCheck && !entry.cursorExhausted) {
            if (Object.hasOwn(entry, 'inUseMemBytes')) {
                foundInUseMem = true;
                assert.gt(
                    entry.inUseMemBytes,
                    0,
                    "Expected inUseMemBytes to be nonzero in getMore: " + tojson(profilerEntries));
                peakInUseMem = Math.max(peakInUseMem, entry.inUseMemBytes);
            }
        }

        assert(entry.hasOwnProperty("maxUsedMemBytes"),
               `Missing maxUsedMemBytes in profiler entry: ${tojson(profilerEntries)}`);
        assert.gte(entry.maxUsedMemBytes,
                   peakInUseMem,
                   `maxUsedMemBytes (${entry.maxUsedMemBytes}) should be >= inUseMemBytes peak (${
                       peakInUseMem}) at this point: ` +
                       tojson(entry));
    }

    if (!verifyOptions.skipInUseMemBytesCheck) {
        assert(foundInUseMem, "Expected to find inUseMemBytes at least once in profiler entries");
    }

    // No memory is currently in use because the cursor is exhausted.
    const exhaustedEntry = profilerEntries.find(entry => entry.cursorExhausted);
    assert(
        exhaustedEntry,
        "Expected to find a profiler entry with cursorExhausted: true: " + tojson(profilerEntries));
    if (verifyOptions.checkInUseMemBytesResets && !verifyOptions.skipInUseMemBytesCheck) {
        assert(
            !exhaustedEntry.hasOwnProperty("inUseMemBytes"),
            "inUseMemBytes should not be present in the final getMore since the cursor is exhausted:" +
                tojson(exhaustedEntry));
    }
}

function verifyExplainMetrics({db, collName, pipeline, stageName, featureFlagEnabled, numStages}) {
    const explainRes = db[collName].explain("executionStats").aggregate(pipeline);

    // If a query uses sbe, the explain version will be 2.
    const isSbeExplain = explainRes.explainVersion === "2";
    const stageKey = isSbeExplain ? stageName : '$' + stageName;

    function getStagesFromExplain(explainRes, stageKey) {
        let stages = getAggPlanStages(explainRes, stageKey);
        // Even if SBE is enabled, there are some stages that are not supported in SBE and will
        // still run on classic. We should also check for the classic pipeline stage name.
        if (isSbeExplain && stages.length == 0) {
            stages = getAggPlanStages(explainRes, '$' + stageName);
        }
        assert.eq(stages.length,
                  numStages,
                  " Found " + stages.length + " but expected to find " + numStages + " " +
                      stageKey + " stages " +
                      "in explain: " + tojson(explainRes));
        return stages;
    }

    function assertNoMemoryMetricsInStages(explainRes, stageKey) {
        let stages = getStagesFromExplain(explainRes, stageKey);
        for (let stage of stages) {
            assert(!stage.hasOwnProperty("maxUsedMemBytes"),
                   `Unexpected maxUsedMemBytes in ${stageKey} stage: ` + tojson(explainRes));
        }
    }

    function assertHasMemoryMetricsInStages(explainRes, stageKey) {
        let stages = getStagesFromExplain(explainRes, stageKey);
        for (let stage of stages) {
            assert(stage.hasOwnProperty("maxUsedMemBytes"),
                   `Expected maxUsedMemBytes in ${stageKey} stage: ` + tojson(explainRes));
            // TODO SERVER-106000 Remove explicit check for $_internalSetWindowFields.
            if (stageKey != "$_internalSetWindowFields") {
                assert.gt(stage.maxUsedMemBytes,
                          0,
                          `Expected maxUsedMemBytes to be positive in ${stageKey} stage: ` +
                              tojson(explainRes));
            }
        }
    }

    if (!featureFlagEnabled) {
        jsTestLog(
            "Test that memory metrics do not appear in the explain output when the feature flag is off.");
        assert(!explainRes.hasOwnProperty("maxUsedMemBytes"),
               "Unexpected maxUsedMemBytes in explain: " + tojson(explainRes));

        // Memory usage metrics do not appear in the stage's statistics. Verify that the stage
        // exists in the explain output.
        assertNoMemoryMetricsInStages(explainRes, stageKey);
        return;
    }

    jsTestLog(
        "Test that memory usage metrics appear in the explain output when the feature flag is on.");

    // Memory usage metrics should appear in the top-level explain for unsharded explains.
    if (!explainRes.hasOwnProperty("shards")) {
        assert(explainRes.hasOwnProperty("maxUsedMemBytes"),
               "Expected maxUsedMemBytes in explain: " + tojson(explainRes));
        assert.gt(explainRes.maxUsedMemBytes,
                  0,
                  "Expected maxUsedMemBytes to be positive: " + tojson(explainRes));
    }

    // Memory usage metrics appear within the stage's statistics.
    assertHasMemoryMetricsInStages(explainRes, stageKey);

    jsTestLog(
        "Test that memory usage metrics do not appear in the explain output when the verbosity is lower than executionStats.");
    const explainQueryPlannerRes = db[collName].explain("queryPlanner").aggregate(pipeline);
    assert(!explainQueryPlannerRes.hasOwnProperty("maxUsedMemBytes"),
           "Unexpected maxUsedMemBytes in explain: " + tojson(explainQueryPlannerRes));
    // SBE stage metrics aren't outputted in queryPlanner explain, so checking
    // the stageKey may result in no stages.
    if (!isSbeExplain) {
        assertNoMemoryMetricsInStages(explainQueryPlannerRes, stageKey);
    }
}

/**
 * For a given pipeline, verify that memory tracking statistics are correctly reported to
 * the slow query log, system.profile, and explain("executionStats").
 */
export function runMemoryStatsTest({
    db,
    collName,
    commandObj,
    stageName,
    expectedNumGetMores,
    skipInUseMemBytesCheck = false,
    // TODO SERVER-105637 Remove this param.
    checkInUseMemBytesResets = true
}) {
    assert("pipeline" in commandObj, "Command object must include a pipeline field.");
    assert("comment" in commandObj, "Command object must include a comment field.");
    assert("cursor" in commandObj, "Command object must include a cursor field.");
    assert("allowDiskUse" in commandObj, "Command object must include allowDiskUse field.");
    assert("aggregate" in commandObj, "Command object must include an aggregate field.");

    const featureFlagEnabled = FeatureFlagUtil.isPresentAndEnabled(db, "QueryMemoryTracking");
    jsTestLog("QueryMemoryTracking feature flag is " + featureFlagEnabled);

    if (skipInUseMemBytesCheck) {
        jsTestLog("Skipping inUseMemBytes checks");
    }

    // Log every operation.
    db.setProfilingLevel(2, {slowms: -1});

    const logLines = runPipelineAndGetDiagnostics(
        {db: db, collName: collName, commandObj: commandObj, source: "log"});
    const verifyOptions = {
        expectedNumGetMores: expectedNumGetMores,
        featureFlagEnabled: featureFlagEnabled,
        checkInUseMemBytesResets: checkInUseMemBytesResets,
        skipInUseMemBytesCheck: skipInUseMemBytesCheck,
    };
    verifySlowQueryLogMetrics({
        logLines: logLines,
        verifyOptions: verifyOptions,
    });

    const profilerEntries = runPipelineAndGetDiagnostics(
        {db: db, collName: collName, commandObj: commandObj, source: "profiler"});
    verifyProfilerMetrics({
        profilerEntries: profilerEntries,
        verifyOptions: verifyOptions,
    });

    verifyExplainMetrics({
        db: db,
        collName: collName,
        pipeline: commandObj.pipeline,
        stageName: stageName,
        featureFlagEnabled: featureFlagEnabled,
        numStages: 1
    });
}

/**
 * For a given pipeline in a sharded cluster, verify that memory tracking statistics are correctly
 * reported to the slow query log and explain("executionStats"). We don't check profiler metrics as
 * the profiler doesn't exist for mongos so we don't test it here.
 */
export function runShardedMemoryStatsTest({
    db,
    collName,
    commandObj,
    stageName,
    expectedNumGetMores,
    numShards,
    skipExplain = false,  // Some stages will execute on the merging part of the pipeline and will
                          // not appear in the shards' explain output.
    skipInUseMemBytesCheck = false
}) {
    assert("pipeline" in commandObj, "Command object must include a pipeline field.");
    assert("comment" in commandObj, "Command object must include a comment field.");
    assert("cursor" in commandObj, "Command object must include a cursor field.");
    assert("allowDiskUse" in commandObj, "Command object must include allowDiskUse field.");
    assert("aggregate" in commandObj, "Command object must include an aggregate field.");

    const featureFlagEnabled = FeatureFlagUtil.isPresentAndEnabled(db, "QueryMemoryTracking");
    jsTestLog("QueryMemoryTracking feature flag is " + featureFlagEnabled);

    // Record every operation in the slow query log.
    db.setProfilingLevel(0, {slowms: -1});
    const logLines = runPipelineAndGetDiagnostics(
        {db: db, collName: collName, commandObj: commandObj, source: "log"});
    const verifyOptions = {
        expectedNumGetMores: expectedNumGetMores,
        featureFlagEnabled: featureFlagEnabled,
        skipInUseMemBytesCheck: skipInUseMemBytesCheck,
    };
    verifySlowQueryLogMetrics({
        logLines: logLines,
        verifyOptions: verifyOptions,
    });

    if (skipExplain) {
        jsTestLog("Skipping explain metrics verification");
        return;
    }

    verifyExplainMetrics({
        db: db,
        collName: collName,
        pipeline: commandObj.pipeline,
        stageName: stageName,
        featureFlagEnabled: featureFlagEnabled,
        numStages: numShards
    });
}
