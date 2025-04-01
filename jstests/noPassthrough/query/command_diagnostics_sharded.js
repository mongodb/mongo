/**
 * Test that tassert during CRUD command execution on mongos will log diagnostics about the query.
 *
 * This test expects collections to persist across a restart.
 * @tags: [requires_persistence]
 */
import {
    assertOnDiagnosticLogContents,
    getDiagnosticLogs,
    getQueryPlannerAlwaysFailsWithNamespace,
    runWithFailpoint
} from "jstests/libs/query/command_diagnostic_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {setParameter} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

const hasEnterpriseModule = getBuildInfo().modules.includes("enterprise");
const dbName = "test";
const collName = jsTestName();
const ns = dbName + "." + collName;

let resetFn;

// We should always be running the comands below against mongos, but depending on which node we are
// asserting has the right ScopedDebugInfo (mongos or mongod) we need to keep track of a
// separate connection to set the failpoint on and read logs from.
let mongosConn;
let connToCheckLogs;

/**
 * Runs 'command' with the queryPlannerAlwaysFails failpoint enabled, finds the resulting
 * ScopedDebugInfo diagnostic log line, and asserts that it contains 'expectedDiagnosticInfo'.
 */
function runTest({
    description,
    command,
    expectedDiagnosticInfo,
    redact = false,
}) {
    // Can't run cases that depend on log redaction without the enterprise module.
    if (!hasEnterpriseModule && redact) {
        return;
    }

    const mongosDb = mongosConn.getDB(dbName);
    const checkLogsDb = connToCheckLogs.getDB(dbName);
    if (hasEnterpriseModule) {
        assert.commandWorked(
            connToCheckLogs.adminCommand({setParameter: 1, redactClientLogData: redact}));
    }

    // In addition to the particular diagnostic info expected per test case, all commands should
    // include the following.
    expectedDiagnosticInfo = expectedDiagnosticInfo.concat([
        "millis:",
        "locks: {}",
        "flowControl: {}",
    ]);
    const {failpointName, failpointOpts, errorCode} = getQueryPlannerAlwaysFailsWithNamespace(ns);

    // If the knob for diagnostic logging is disabled, ensure that we do not see any diagnostic
    // logging.
    setParameter(checkLogsDb, "enableDiagnosticLogging", false);
    jsTestLog("Running test case with knob disabled: " +
              tojson({description, command, expectedDiagnosticInfo, redact}));
    runWithFailpoint(checkLogsDb, failpointName, failpointOpts, () => {
        // BulkWrites don't fail if sub-operations fail, but they would still generate the
        // diagnostic log if the knob was enabled.
        if (!command.bulkWrite) {
            assert.commandFailedWithCode(mongosDb.runCommand(command), errorCode, description);
        } else {
            assert.commandWorked(mongosDb.adminCommand(command), description);
        }
    });

    const commandDiagnostics =
        getDiagnosticLogs({description: description, logFile: connToCheckLogs.fullOptions.logFile});
    assert.eq(commandDiagnostics.length,
              0,
              `${description}: found an unexpected log line containing command diagnostics`);

    // Now enable the knob and ensure that we do see the expected logs.
    setParameter(checkLogsDb, "enableDiagnosticLogging", true);
    jsTestLog("Running test case with knob enabled:" +
              tojson({description, command, expectedDiagnosticInfo, redact}));
    runWithFailpoint(checkLogsDb, failpointName, failpointOpts, () => {
        // BulkWrites don't fail if sub-operations fail, but they still generate the diagnostic log.
        if (!command.bulkWrite) {
            assert.commandFailedWithCode(mongosDb.runCommand(command), errorCode, description);
        } else {
            assert.commandWorked(mongosDb.adminCommand(command), description);
        }
    });

    assertOnDiagnosticLogContents({
        description: description,
        logFile: connToCheckLogs.fullOptions.logFile,
        expectedDiagnosticInfo: expectedDiagnosticInfo
    });

    resetFn();
}

// The queries below require shard targeting, which uses query planning as a sub-routine. Thus,
// both mongos and the shards will hit the queryPlannerAlwaysFails failpoint when executing these
// queries.
const query = {
    a: 1,
    b: 1
};
const shardKey = {
    a: 1,
    b: 1,
    c: 1
};

const defaultExpCtxLog = [
    'ExpCtxDiagnostics\: {collator: ',
    'uuid: ',
    'needsMerge: ',
    'allowDiskUse: ',
    'isMapReduceCommand: ',
    'inLookup: ',
    'inUnionWith: ',
    'forcePlanCache: ',
    'sbeCompatibility: ',
    'sbeGroupCompatibility: ',
    'sbeWindowCompatibility: ',
    'sbePipelineCompatibility: ',
    'subPipelineDepth: '
];

const shardKeyLog = `\'shardKeyPattern\': { a: 1.0, b: 1.0, c: 1.0 }`;

function runTests(onMongos = false) {
    // Find
    runTest({
        description: "find",
        command: {find: collName, filter: query, limit: 1},
        expectedDiagnosticInfo: [
            "{\'currentOp\': { ",
            `ns: \\"${ns}\\"`,
            `\'opDescription\': { find: \\"${collName}\\", filter: { a: 1.0, b: 1.0 }, limit: 1`,
            ...defaultExpCtxLog,
            shardKeyLog,
        ],
    });
    runTest({
        description: "find with log redaction",
        command: {find: collName, filter: query, limit: 1},
        redact: true,
        expectedDiagnosticInfo: [
            '{\'currentOp\': { op: \\"###\\", ns: \\"###\\"',
            '\'opDescription\': { find: \\"###\\", filter: { a: \\"###\\", b: \\"###\\" }, limit: \\"###\\"',
            ...defaultExpCtxLog,
            shardKeyLog,
        ],
    });

    // Aggregate
    runTest({
        description: "aggregate",
        command: {aggregate: collName, pipeline: [{$match: query}], cursor: {}},
        expectedDiagnosticInfo: [
            `{\'currentOp\': { op: \\"command\\", ns: \\"${ns}\\"`,
            `\'opDescription\': { aggregate: \\"${
                collName}\\", pipeline: [ { $match: { a: 1.0, b: 1.0 } } ]`,
            ...defaultExpCtxLog,
            shardKeyLog,
        ]
    });

    // Count
    runTest({
        description: "count",
        command: {count: collName, query: query},
        expectedDiagnosticInfo: [
            `{\'currentOp\': { op: \\"command\\", ns: \\"${ns}\\"`,
            `\'opDescription\': { count: \\"${collName}\\", query: { a: 1.0, b: 1.0 }`,
            ...defaultExpCtxLog,
            shardKeyLog,
        ]
    });

    // Distinct
    runTest({
        description: "distinct",
        command: {distinct: collName, key: "a", query: query},
        expectedDiagnosticInfo: [
            `{\'currentOp\': { op: \\"command\\", ns: \\"${ns}\\"`,
            `\'opDescription\': { distinct: \\"${
                collName}\\", key: \\"a\\", query: { a: 1.0, b: 1.0 }`,
            ...defaultExpCtxLog,
            shardKeyLog,
        ]
    });

    // MapReduce
    runTest({
        description: "mapReduce",
        command: {mapReduce: collName, map: () => emit(0, 0), reduce: () => 1, out: {inline: 1}},
        expectedDiagnosticInfo: [
            `{\'currentOp\': { op: \\"command\\", ns: \\"${ns}\\"`,
            'isMapReduceCommand: true',
            ...defaultExpCtxLog,
            shardKeyLog,
        ]
    });

    // Delete
    // TODO SERVER-98444: We should be able to see the delete sub-operations under "opDescription".
    runTest({
        description: 'delete',
        command: {
            delete: collName,
            deletes: [{q: query, limit: 1}],
        },
        expectedDiagnosticInfo: [
            `{\'currentOp\': { op: \\"remove\\", ns: \\"${ns}\\"`,
            '\'opDescription\': ',
            shardKeyLog,
        ]
    });

    // Update
    // TODO SERVER-98444: We should be able to see the update sub-operations under "opDescription".
    runTest({
        description: 'update with simple filter',
        command: {
            update: collName,
            updates: [{q: query, u: {a: 2, b: 2}}],
        },
        expectedDiagnosticInfo: [
            `{\'currentOp\': { op: \\"update\\", ns: \\"${ns}\\"`,
            '\'opDescription\': ',
            shardKeyLog,
        ]
    });

    // FindAndModify
    runTest({
        description: 'findAndModify remove',
        command: {
            findAndModify: collName,
            query: query,
            remove: true,
        },
        expectedDiagnosticInfo: [
            `{\'currentOp\': { op: \\"command\\", ns: \\"${ns}\\"`,
            `\'opDescription\': { findAndModify: \\"${
                collName}\\", query: { a: 1.0, b: 1.0 }, remove: true`,
            shardKeyLog,
        ]
    });

    // BulkWrite
    runTest({
        command: {
            bulkWrite: 1,
            ops: [
                {update: 0, filter: query, updateMods: {b: 1}},
                {update: 1, filter: query, updateMods: {a: 1}}
            ],
            nsInfo: [
                {ns: "test.differentNamespace"},
                {ns: `test.${jsTestName()}`},
            ],
        },
        description: 'bulkWrite fails',
        expectedDiagnosticInfo: [
            `{\'currentOp\': { op: \\"bulkWrite\\"`,
            // Ensure sub-operations are included in the diagnostic log.
            'update: 0',
            'filter: { a: 1.0, b: 1.0 }',
            'updateMods: { a: 1.0 }',
            onMongos ? (`'test.differentNamespace': omitted: collection isn't sharded`,
                        `'test.command_diagnostics_sharded': { a: 1.0, b: 1.0, c: 1.0 }`)
                     : shardKeyLog,
        ]
    });

    // Explain
    runTest({
        description: "explain find",
        command: {explain: {find: collName, filter: query, limit: 1}},
        expectedDiagnosticInfo: [
            `{\'currentOp\': { op: \\"command\\", ns: \\"test.`,
            `\'opDescription\': { explain: { find: \\"${
                collName}\\", filter: { a: 1.0, b: 1.0 }, limit: 1`,
            ...defaultExpCtxLog,
            shardKeyLog,
        ],
    });
    runTest({
        description: "explain aggregate",
        command: {
            explain:
                {aggregate: collName, pipeline: [{$match: query}, {$unwind: "$arr"}], cursor: {}}
        },
        expectedDiagnosticInfo: [
            `{\'currentOp\': { op: \\"command\\", ns: \\"test.`,
            `\'opDescription\': { explain: { aggregate: \\"${
                collName}\\", pipeline: [ { $match: { a: 1.0, b: 1.0 } }, { $unwind: `,
            ...defaultExpCtxLog,
            shardKeyLog,
        ]
    });
    runTest({
        description: "explain distinct",
        command: {explain: {distinct: collName, key: "a", query: query}},
        expectedDiagnosticInfo: [
            `{\'currentOp\': { op: \\"command\\", ns: \\"test.`,
            `\'opDescription\': { explain: { distinct: \\"${
                collName}\\", key: \\"a\\", query: { a: 1.0, b: 1.0 } }`,
            ...defaultExpCtxLog,
            shardKeyLog,
        ]
    });
    runTest({
        description: "explain count",
        command: {explain: {count: collName, query: query}},
        expectedDiagnosticInfo: [
            `{\'currentOp\': { op: \\"command\\", ns: \\"test.`,
            `\'opDescription\': { explain: { count: \\"${collName}\\", query: { a: 1.0, b: 1.0 }`,
            shardKeyLog,
        ]
    });
}

jsTestLog("Testing tassert log diagnostics on mongos");
{
    const st =
        new ShardingTest({mongos: 1, shards: 1, other: {mongosOptions: {useLogFiles: true}}});
    const db = st.s.getDB(dbName);
    const coll = db[collName];
    coll.drop();
    assert.commandWorked(coll.createIndex(shardKey));
    assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: shardKey}));

    // We expect a non-zero exit code due to tassert triggered. Restarting will also clear the
    // log files that we grep in assertOnDiagnosticLogContents for the next test case.
    resetFn = () => {
        st.restartMongos(0, st.s0.opts, {allowedExitCode: MongoRunner.EXIT_ABRUPT});
        mongosConn = st.s;
        connToCheckLogs = st.s;
    };
    mongosConn = st.s;
    connToCheckLogs = st.s;
    runTests(true /* onMongos */);

    st.stop();
}

jsTestLog("Testing tassert log diagnostics on shards");
{
    const st = new ShardingTest({shards: [{useLogFiles: true}], mongos: 1});
    const db = st.s.getDB(dbName);
    const coll = db[collName];
    coll.drop();
    assert.commandWorked(coll.createIndex(shardKey));
    assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: shardKey}));

    // We expect a non-zero exit code due to tassert triggered. Restarting will also clear the
    // log files that we grep in assertOnDiagnosticLogContents for the next test case.
    resetFn = () => {
        st.rs0.restart(st.rs0.getPrimary(), {allowedExitCode: MongoRunner.EXIT_ABRUPT});
        connToCheckLogs = st.rs0.getPrimary();
    };
    mongosConn = st.s;
    connToCheckLogs = st.rs0.getPrimary();
    runTests();

    st.stop();
}
