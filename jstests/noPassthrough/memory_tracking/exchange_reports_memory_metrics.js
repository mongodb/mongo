/**
 * Tests that, when the memory tracking feature flag is enabled, memory tracking statistics are
 * reported to the slow query log, system.profile, and explain("executionStats") for the stages that
 * feed into an $_internalExchange stage.
 *
 * @tags: [
 *   requires_profiling,
 *   requires_getmore,
 *   # The test queries the system.profile collection so it is not compatible with initial sync
 *   # since an initial sync may insert unexpected operations into the profile collection.
 *   queries_system_profile_collection,
 *   # The test runs the profile and getLog commands, which are not supported in Serverless.
 *   command_not_supported_in_serverless,
 *   assumes_against_mongod_not_mongos,
 *   requires_fcv_82,
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {iterateMatchingLogLines} from "jstests/libs/log.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {verifyProfilerMetrics, verifySlowQueryLogMetrics} from "jstests/libs/query/memory_tracking_utils.js";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");

const db = conn.getDB("test");
db.dropDatabase();
const collName = jsTestName();
const coll = db[collName];
db[collName].drop();

// Log every operation.
db.setProfilingLevel(2, {slowms: -1});

const featureFlagEnabled = FeatureFlagUtil.isPresentAndEnabled(db, "QueryMemoryTracking");
const nConsumers = NumberInt(3);
const docCount = nConsumers * 100;
for (let i = 0; i < docCount; ++i) {
    assert.commandWorked(coll.insertOne({_id: i, b: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}));
}

let aggCmd = {
    aggregate: collName,
    pipeline: [
        {
            $group: {
                _id: {$concat: [{$toString: "$_id"}, "X"]},
                v0: {$first: "$_id"},
                v1: {$first: "$b"},
            },
        },
    ],
    cursor: {batchSize: 0},
    exchange: {
        "policy": "roundrobin",
        "consumers": nConsumers,
        "orderPreserving": false,
        "bufferSize": NumberInt(128),
        "key": {},
    },
};

// Create explicit session to avoid session mismatch issues.
const session = db.getMongo().startSession();
const sessionDb = session.getDatabase(db.getName());

// Run the aggregate command and get cursor IDs.
const aggResult = assert.commandWorked(sessionDb.runCommand(aggCmd));

// Extract cursor IDs from the result.
assert(aggResult.cursors, "Expected cursors array in aggregate result");
assert.eq(aggResult.cursors.length, nConsumers, `Expected ${nConsumers} cursors`);

let cursorIds = [];
for (let i = 0; i < aggResult.cursors.length; i++) {
    const cursorId = aggResult.cursors[i].cursor.id;
    cursorIds.push(cursorId);
}

// Spawn parallel shells to exhaust each cursor.
let parallelShells = [];
for (let i = 0; i < cursorIds.length; i++) {
    const cursorId = cursorIds[i];
    const shellFunction = function (args) {
        const batchSize = NumberInt(10);
        let totalDocs = 0;
        let batchCount = 0;

        while (true) {
            const getMoreResult = db.runCommand({
                getMore: args.cursorId,
                collection: args.collName,
                batchSize: batchSize,
                lsid: args.sessionId,
            });

            // Assert that getMore succeeded
            assert.commandWorked(getMoreResult, `Shell ${args.shellIndex} getMore failed for cursor ${args.cursorId}`);

            const batch = getMoreResult.cursor.nextBatch;
            totalDocs += batch.length;
            batchCount++;

            // If no more documents, cursor is exhausted
            if (batch.length === 0) {
                break;
            }
        }

        // Assert that we actually got all documents from this cursor
        assert.eq(
            totalDocs,
            args.docCount,
            `Shell ${args.shellIndex} should have retrieved ${args.docCount} documents from cursor ${args.cursorId}`,
        );

        const expectedRequests = args.docCount / batchSize;
        let logLines = [];
        assert.soon(() => {
            const globalLog = assert.commandWorked(db.adminCommand({getLog: "global"}));
            logLines = [...iterateMatchingLogLines(globalLog.log, {msg: "Slow query", cursorid: args.cursorId})];
            return logLines.length >= expectedRequests;
        }, `Failed to find 10 log lines for cursorid: ${args.cursorId.toString()}`);

        // Verify memory metrics based on shell index. Only the first shell will have any metrics.
        // We accumulate memory metrics into consumer 0 only, in order to avoid a data race with the
        // other consumers.
        const verifyOptions = {
            expectMemoryMetrics: args.featureFlagEnabled && args.shellIndex === 0,
            expectedNumGetMores: logLines.length,
            skipFindInitialRequest: true,
            // The peak metric may not show up for early getMore() logs, since memory metrics are
            // only propagated up to CurOp when the first consumer happens to execute the
            // subpipeline. We are guaranteed to have it when dispose() is called after the pipeline
            // is exhausted though.
            allowMissingPeakMetric: true,
        };

        verifySlowQueryLogMetrics({
            logLines: logLines,
            verifyOptions: verifyOptions,
        });

        let profilerEntries = [];
        assert.soon(() => {
            profilerEntries = db.system.profile.find({"cursorid": args.cursorId}).toArray();
            return profilerEntries.length >= expectedRequests;
        }, `Failed to find a profiler entry for cursorid: ${args.cursorId}`);

        verifyProfilerMetrics({
            profilerEntries: profilerEntries,
            verifyOptions: verifyOptions,
        });
    };

    // Start parallel shell
    const parallelScript = `import {iterateMatchingLogLines} from "jstests/libs/log.js";
import {verifySlowQueryLogMetrics, verifyProfilerMetrics} from "jstests/libs/query/memory_tracking_utils.js";
${funWithArgs(shellFunction, {
    featureFlagEnabled: featureFlagEnabled,
    collName: collName,
    cursorId: cursorId,
    shellIndex: i,
    sessionId: session.getSessionId(),
    docCount: docCount / nConsumers,
})}`;
    const shell = startParallelShell(parallelScript, conn.port);

    parallelShells.push(shell);
}

// Wait for all parallel shells to complete.
for (let i = 0; i < parallelShells.length; i++) {
    parallelShells[i]();
}

jsTest.log.info("All parallel shells completed successfully");

session.endSession();

db[collName].drop();
MongoRunner.stopMongod(conn);
