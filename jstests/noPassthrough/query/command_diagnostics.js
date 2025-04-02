/**
 * Test that tassert during CRUD command execution will log diagnostics about the query.
 */
import {
    assertOnDiagnosticLogContents,
    failAllInserts,
    getDiagnosticLogs,
    planExecutorAlwaysFails,
    runWithFailpoint
} from "jstests/libs/query/command_diagnostic_utils.js";
import {setParameter} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

// This test triggers an unclean shutdown, which may cause inaccurate fast counts.
TestData.skipEnforceFastCountOnValidate = true;

const hasEnterpriseModule = getBuildInfo().modules.includes("enterprise");

const collName = jsTestName();
const outColl = collName + "_out";

function setup(conn, targetDb) {
    const db = conn.getDB(targetDb);
    const coll = db[collName];
    coll.drop();
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({b: 1}));
    for (let i = 0; i < 200; i++) {
        assert.commandWorked(coll.insert({a: 1, b: 1}));
    }
    return {db, coll};
}

/**
 * Asserts that given diagnostic info is printed when a command tasserts.
 *
 * description - Description of this test case.
 * command - The command to run.
 * expectedDiagnosticInfo -  List of strings that are expected to be in the diagnostic log.
 * redact - Whether to enable log redaction before dumping diagnostic info.
 * errorCode - The expected error code(s) for the given command and failpoint.
 * failpointName - Which failpoint to enable when running 'command'.
 * failpointOpts - Additional options for the failpoint.
 * targetDb - Name of the database to run 'command' against.
 */
function runTest({
    description,
    command,
    expectedDiagnosticInfo,
    redact,
    errorCode,
    failpointName,
    failpointOpts,
    targetDb = "test",
}) {
    // Can't run cases that depend on log redaction without the enterprise module.
    if (!hasEnterpriseModule && redact) {
        return;
    }

    // Each test case needs to start a new mongod to clear the previous logs.
    const conn = MongoRunner.runMongod({useLogFiles: true});
    const {db, coll} = setup(conn, targetDb);

    if (hasEnterpriseModule) {
        assert.commandWorked(db.adminCommand({setParameter: 1, redactClientLogData: redact}));
    }

    // If the knob for diagnostic logging is disabled, ensure that we do not see any diagnostic
    // logging.
    setParameter(conn, "enableDiagnosticLogging", false);
    runWithFailpoint(db, failpointName, failpointOpts, () => {
        jsTestLog("Running test case with knob disabled: " + description);
        if (errorCode) {
            assert.commandFailedWithCode(db.runCommand(command), errorCode, description);
        } else {
            assert.commandWorked(db.runCommand(command), description);
        }
    });

    const commandDiagnostics =
        getDiagnosticLogs({description: description, logFile: conn.fullOptions.logFile});
    assert.eq(commandDiagnostics.length,
              0,
              `${description}: found an unexpected log line containing command diagnostics`);

    // Now enable the knob and ensure that we do see the expected logs.
    setParameter(conn, "enableDiagnosticLogging", true);
    runWithFailpoint(db, failpointName, failpointOpts, () => {
        jsTestLog("Running test case with knob enabled:" + description);
        if (errorCode) {
            assert.commandFailedWithCode(db.runCommand(command), errorCode, description);
        } else {
            assert.commandWorked(db.runCommand(command), description);
        }
    });

    assertOnDiagnosticLogContents({
        description: description,
        logFile: conn.fullOptions.logFile,
        expectedDiagnosticInfo: expectedDiagnosticInfo
    });

    // We expect a non-zero exit code due to tassert triggered.
    MongoRunner.stopMongod(conn, null, {allowedExitCode: MongoRunner.EXIT_ABRUPT});
}

const commonDiagnostics = [
    "millis:",
    // We specifically don't want to log any information that requires synchronization or disk
    // access.
    "locks: {}",
    "flowControl: {}",
];
const omittedShardKeyLog = "omitted: collection isn't sharded";
const commonQueryDiagnostics = [
    ...commonDiagnostics,
    "planCacheShapeHash:",
    "planCacheKey:",
    "queryShapeHash:",
    "queryFramework:",
    "planningTimeMicros:",
    'planSummary: \\"IXSCAN',
    omittedShardKeyLog,
];
const expectedExplainCollscanDiagnostic =
    ["'winningPlan': ", 'stage: \\"IXSCAN\\"', 'stage: \\"FETCH\\"', "'executionStats': "];

const failAllRemoves = {
    failpointName: "failAllRemoves",
    failpointOpts: {'tassert': true},
    errorCode: [9276703, 9276704],
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

// Test the find command.
runTest({
    ...planExecutorAlwaysFails,
    description: "simple find",
    command: {find: collName, filter: {a: 1, b: 1}, limit: 1},
    expectedDiagnosticInfo: [
        ...commonQueryDiagnostics,
        `{\'currentOp\': { op: \\"query\\", ns: \\"test.${jsTestName()}\\"`,
        '\'opDescription\': { find: \\"command_diagnostics\\", filter: { a: 1.0, b: 1.0 }, limit: 1.0',
        ...expectedExplainCollscanDiagnostic,
        ...defaultExpCtxLog,
    ],
});

runTest({
    ...planExecutorAlwaysFails,
    description: "simple find with log redaction",
    command: {find: collName, filter: {a: 1, b: 1}, limit: 1},
    redact: true,
    expectedDiagnosticInfo: [
        '{\'currentOp\': { op: \\"###\\", ns: \\"###\\"',
        '\'opDescription\': { find: \\"###\\", filter: { a: \\"###\\", b: \\"###\\" }, limit: \\"###\\"',
        'planSummary: \\"###\\"',
        // Explain diagnostics
        "'winningPlan': ",
        'stage: \\"###\\"',
        "'executionStats': ",
        ...defaultExpCtxLog,
        omittedShardKeyLog,
    ],
});

// Test the aggregate command.
runTest({
    ...planExecutorAlwaysFails,
    description: "agg with a simple $match",
    command: {aggregate: collName, pipeline: [{$match: {a: 1, b: 1}}], cursor: {}},
    expectedDiagnosticInfo: [
        ...commonQueryDiagnostics,
        `{\'currentOp\': { op: \\"command\\", ns: \\"test.${jsTestName()}\\"`,
        '\'opDescription\': { aggregate: \\"command_diagnostics\\", pipeline: [ { $match: { a: 1.0, b: 1.0 } } ]',
        ...expectedExplainCollscanDiagnostic,
        ...defaultExpCtxLog,
    ]
});
const outCmd = {
    aggregate: collName,
    pipeline: [{$match: {a: 1, b: 1}}, {$out: outColl}],
    cursor: {}
};
const outCmdDescription =
    `\'opDescription\': { aggregate: \\"command_diagnostics\\", pipeline: [ { $match: { a: 1.0, b: 1.0 } }, { $out: \\"${
        outColl}\\" } ]`;
runTest({
    ...planExecutorAlwaysFails,
    description: "agg with $out, planExecutor fails",
    command: outCmd,
    expectedDiagnosticInfo: [
        ...commonQueryDiagnostics,
        outCmdDescription,
        '{\'currentOp\': { op: \\"command\\", ns: \\"test.tmp.agg_out.',
        ...expectedExplainCollscanDiagnostic,
        ...defaultExpCtxLog,
    ]
});
runTest({
    ...failAllInserts,
    description: "agg with $out, insert fails",
    command: outCmd,
    expectedDiagnosticInfo: [
        ...commonQueryDiagnostics,
        outCmdDescription,
        '{\'currentOp\': { op: \\"insert\\", ns: \\"test.tmp.agg_out.',
        'ninserted: 0',
        // Not expecting any explain diagnostics for inserts.
        ...defaultExpCtxLog,
    ]
});

// Test the count command.
runTest({
    ...planExecutorAlwaysFails,
    description: "count",
    command: {count: collName, query: {a: 1, b: 1}},
    expectedDiagnosticInfo: [
        ...commonDiagnostics,
        `{\'currentOp\': { op: \\"command\\", ns: \\"test.${jsTestName()}\\"`,
        '\'opDescription\': { count: \\"command_diagnostics\\", query: { a: 1.0, b: 1.0 }',
        ...expectedExplainCollscanDiagnostic,
        ...defaultExpCtxLog,
        omittedShardKeyLog,
    ]
});

// Test the distinct command.
runTest({
    ...planExecutorAlwaysFails,
    description: "distinct",
    command: {distinct: collName, key: "a", query: {a: 1, b: 1}},
    expectedDiagnosticInfo: [
        ...commonQueryDiagnostics,
        `{\'currentOp\': { op: \\"command\\", ns: \\"test.${jsTestName()}\\"`,
        '\'opDescription\': { distinct: \\"command_diagnostics\\", key: \\"a\\", query: { a: 1.0, b: 1.0 }',
        ...expectedExplainCollscanDiagnostic,
        ...defaultExpCtxLog,
    ]
});

// Test the mapReduce command.
runTest({
    ...planExecutorAlwaysFails,
    description: "mapReduce",
    command: {
        mapReduce: collName,
        map: () => emit(0, 0),
        reduce: () => 1,
        out: {inline: 1},
    },
    expectedDiagnosticInfo: [
        ...commonDiagnostics,
        `{\'currentOp\': { op: \\"command\\", ns: \\"test.${jsTestName()}\\"`,
        '\'opDescription\': { mapReduce: \\"command_diagnostics\\", map: () => emit(0, 0), reduce: () => 1, out: { inline: 1.0 }',
        // Explain diagnostics
        "'winningPlan': ",
        'stage: \\"COLLSCAN\\"',
        "'executionStats': ",
        ...defaultExpCtxLog
    ]
});

// Test the insert command.
runTest({
    ...failAllInserts,
    description: "insert",
    command: {insert: collName, documents: [...Array(25).fill({a: 1, b: 1})], ordered: false},
    expectedDiagnosticInfo: [
        ...commonDiagnostics,
        `{\'currentOp\': { op: \\"insert\\", ns: \\"test.${jsTestName()}\\"`,
        '\'opDescription\': { insert: \\"command_diagnostics\\", ordered: false',
        'ninserted: 0',
        // Not expecting any explain diagnostics for inserts.
        omittedShardKeyLog,
    ]
});

// Test the delete command.
runTest({
    ...planExecutorAlwaysFails,
    description: 'delete',
    command: {
        delete: collName,
        deletes: [{q: {a: 1, b: 1}, limit: 1}],
    },
    expectedDiagnosticInfo: [
        ...commonDiagnostics,
        `{\'currentOp\': { op: \\"remove\\", ns: \\"test.${jsTestName()}\\"`,
        '\'opDescription\': { q: { a: 1.0, b: 1.0 }, limit: 1 }',
        ...expectedExplainCollscanDiagnostic,
        ...defaultExpCtxLog,
        omittedShardKeyLog,
    ]
});

// Test the update command.
runTest({
    ...planExecutorAlwaysFails,
    description: 'update with simple filter',
    command: {
        update: collName,
        updates: [{q: {a: 1, b: 1}, u: {a: 2, b: 2}}],
    },
    expectedDiagnosticInfo: [
        ...commonDiagnostics,
        `{\'currentOp\': { op: \\"update\\", ns: \\"test.${jsTestName()}\\"`,
        '\'opDescription\': { q: { a: 1.0, b: 1.0 }, u: { a: 2.0, b: 2.0 }',
        ...expectedExplainCollscanDiagnostic,
        ...defaultExpCtxLog,
        omittedShardKeyLog,
    ]
});

// Test the findAndModify command.
runTest({
    ...planExecutorAlwaysFails,
    description: 'findAndModify remove',
    command: {
        findAndModify: collName,
        query: {a: 1, b: 1},
        remove: true,
    },
    expectedDiagnosticInfo: [
        ...commonDiagnostics,
        `{\'currentOp\': { op: \\"command\\", ns: \\"test.${jsTestName()}\\"`,
        '\'opDescription\': { findAndModify: \\"command_diagnostics\\", query: { a: 1.0, b: 1.0 }, remove: true',
        ...expectedExplainCollscanDiagnostic,
        ...defaultExpCtxLog,
        omittedShardKeyLog,
    ]
});
runTest({
    ...planExecutorAlwaysFails,
    description: 'findAndModify update',
    command: {
        findAndModify: collName,
        query: {a: 1, b: 1},
        update: {a: 2, b: 2},
    },
    expectedDiagnosticInfo: [
        ...commonDiagnostics,
        `{\'currentOp\': { op: \\"command\\", ns: \\"test.${jsTestName()}\\"`,
        '\'opDescription\': { findAndModify: \\"command_diagnostics\\", query: { a: 1.0, b: 1.0 }, update: { a: 2.0, b: 2.0 }',
        ...expectedExplainCollscanDiagnostic,
        ...defaultExpCtxLog,
        omittedShardKeyLog,
    ]
});

// Test the bulkWrite command.
const bulkWriteTestSpec = {
    targetDb: 'admin',
    command: {
        bulkWrite: 1,
        ops: [
            {insert: 0, document: {a: 0}},
            {update: 0, filter: {a: 1, b: 1}, updateMods: {a: 1}},
            {delete: 0, filter: {a: 1}},
        ],
        nsInfo: [{ns: `test.${jsTestName()}`}],
    },
};
runTest({
    ...failAllRemoves,
    ...bulkWriteTestSpec,
    description: 'bulkWrite, delete fails',
    // The top-level command doesn't fail when a delete fails.
    errorCode: 0,
    expectedDiagnosticInfo: [
        ...commonDiagnostics,
        `{\'currentOp\': { op: \\"bulkWrite\\", ns: \\"test.${jsTestName()}\\"`,
        // Ensure the failing sub-operation is included in the diagnostic log.
        '\'opDescription\': { delete: 0, filter: { a: 1.0 }',
        // Not expecting explain diagnostics here; this failpoint is reached before we have a query
        // plan to explain.
        ...defaultExpCtxLog,
        omittedShardKeyLog,
    ]
});
runTest({
    ...planExecutorAlwaysFails,
    ...bulkWriteTestSpec,
    description: 'bulkWrite, update fails',
    // The top-level command doesn't fail when an update fails.
    errorCode: 0,
    expectedDiagnosticInfo: [
        ...commonDiagnostics,
        `{\'currentOp\': { op: \\"bulkWrite\\", ns: \\"test.${jsTestName()}\\"`,
        // Ensure the failing sub-operation is included in the diagnostic log.
        '\'opDescription\': { update: 0, filter: { a: 1.0, b: 1.0 }, multi: false, updateMods: { a: 1.0 }',
        // Explain diagnostics
        "'winningPlan': ",
        'stage: \\"UPDATE\\"',
        "'executionStats': ",
        ...defaultExpCtxLog,
        omittedShardKeyLog,
    ]
});
runTest({
    ...failAllInserts,
    ...bulkWriteTestSpec,
    description: 'bulkWrite, insert fails',
    expectedDiagnosticInfo: [
        ...commonDiagnostics,
        `{\'currentOp\': { op: \\"bulkWrite\\", ns: \\"test.${jsTestName()}\\"`,
        // Ensure the failing sub-operation is included in the diagnostic log.
        '\'opDescription\': { insert: 0, documents: [ { a: 0.0 } ] }',
        // Not expecting any explain diagnostics for inserts.
        omittedShardKeyLog,
    ]
});

// Explain
runTest({
    ...planExecutorAlwaysFails,
    description: "explain find",
    command:
        {explain: {find: collName, filter: {a: 1, b: 1}, limit: 1}, verbosity: "executionStats"},
    // The top-level command doesn't fail when the plan executor fails.
    errorCode: 0,
    expectedDiagnosticInfo: [
        `{\'currentOp\': { op: \\"command\\", ns: \\"test.${jsTestName()}\\"`,
        '\'opDescription\': { explain: { find: \\"command_diagnostics\\", filter: { a: 1.0, b: 1.0 }, limit: 1.0',
        ...expectedExplainCollscanDiagnostic,
        ...defaultExpCtxLog,
        omittedShardKeyLog,
    ],
});
runTest({
    ...planExecutorAlwaysFails,
    description: "explain aggregate",
    command: {
        explain: {
            aggregate: collName,
            pipeline: [{$match: {a: 1, b: 1}}, {$unwind: "$arr"}],
            cursor: {}
        },
        verbosity: "executionStats"
    },
    expectedDiagnosticInfo: [
        `{\'currentOp\': { op: \\"command\\", ns: \\"test.${jsTestName()}\\"`,
        '\'opDescription\': { explain: { aggregate: \\"command_diagnostics\\", pipeline: [ { $match: { a: 1.0, b: 1.0 } }, { $unwind: \\"$arr\\" } ]',
        ...expectedExplainCollscanDiagnostic,
        ...defaultExpCtxLog,
        omittedShardKeyLog,
    ]
});
runTest({
    ...planExecutorAlwaysFails,
    description: "explain count",
    command: {explain: {count: collName, query: {a: 1, b: 1}}, verbosity: "executionStats"},
    // The top-level command doesn't fail when the plan executor fails.
    errorCode: 0,
    expectedDiagnosticInfo: [
        ...commonDiagnostics,
        `{\'currentOp\': { op: \\"command\\", ns: \\"test.${jsTestName()}\\"`,
        '\'opDescription\': { explain: { count: \\"command_diagnostics\\", query: { a: 1.0, b: 1.0 } }',
        ...expectedExplainCollscanDiagnostic,
        ...defaultExpCtxLog,
        omittedShardKeyLog,
    ]
});
runTest({
    ...planExecutorAlwaysFails,
    description: "explain distinct",
    command:
        {explain: {distinct: collName, key: "a", query: {a: 1, b: 1}}, verbosity: "executionStats"},
    // The top-level command doesn't fail when the plan executor fails.
    errorCode: 0,
    expectedDiagnosticInfo: [
        `{\'currentOp\': { op: \\"command\\", ns: \\"test.${jsTestName()}\\"`,
        '\'opDescription\': { explain: { distinct: \\"command_diagnostics\\", key: \\"a\\", query: { a: 1.0, b: 1.0 } }',
        ...expectedExplainCollscanDiagnostic,
        ...defaultExpCtxLog,
        omittedShardKeyLog,
    ]
});

// Test the getMore command.
{
    const conn = MongoRunner.runMongod({useLogFiles: true});
    const {db, coll} = setup(conn, "test");

    const description = "getMore";
    const {failpointName, failpointOpts, errorCode} = planExecutorAlwaysFails;

    const {cursor} = assert.commandWorked(db.runCommand({find: collName, batchSize: 0}));
    runWithFailpoint(db, failpointName, failpointOpts, () => {
        jsTestLog("Testing getMore");
        assert.commandFailedWithCode(
            db.runCommand({getMore: cursor.id, collection: collName}), errorCode, description);
    });

    // Get e.g. "2233240269355766922" from 'NumberLong("2233240269355766922")'.
    const cursorIdAsString = cursor.id.toString().split('"')[1];
    assertOnDiagnosticLogContents({
        description,
        logFile: conn.fullOptions.logFile,
        expectedDiagnosticInfo: [
            ...commonDiagnostics,
            `{\'currentOp\': { op: \\"getmore\\", ns: \\"test.${jsTestName()}\\"`,
            `\'opDescription\': { getMore: ${
                cursorIdAsString}, collection: \\"command_diagnostics\\"`,
            '\'originatingCommand\': { find: \\"command_diagnostics\\"'
        ]
    });

    // We expect a non-zero exit code due to tassert triggered.
    MongoRunner.stopMongod(conn, null, {allowedExitCode: MongoRunner.EXIT_ABRUPT});
}

// Test that large commands are handled properly.
const bsonMaxUserSize = 16 * 1024 * 1024;
const bsonMaxInternalSize = bsonMaxUserSize + (16 * 1024);

function createLargeObject({inListLength}) {
    // Use a large $in-list, since BSONObj.toString() can automatically truncate strings.
    return {a: {$in: [...Array(inListLength).fill(1)]}, afterIn: "b"};
}

function createLargeObjectCloseToUserLimit() {
    // Internally, BSON arrays are represented as objects (e.g. {'0': val, '1': val, ...,
    // '1000000': val}), which makes coming up with formula to calculate the desired length a
    // bit tedious. The in-list length is chosen such that the size of resulting predicate is as
    // close to the max size as possible.
    const result = createLargeObject({inListLength: (1024 * 1024) + 3676});
    const remainingBytes = bsonMaxUserSize - Object.bsonsize(result);
    assert.eq(remainingBytes, 4);
    return result;
}

function createLargeObjectCloseToInternalLimit() {
    // The in-list length is chosen such that the size of resulting predicate is as close to the
    // max size as possible. See the comment in the above function for further explanation.
    const result = createLargeObject({inListLength: (1024 * 1024) + 4639});
    const remainingBytes = bsonMaxInternalSize - Object.bsonsize(result);
    assert.eq(remainingBytes, 17);
    return result;
}

runTest({
    ...planExecutorAlwaysFails,
    description: "find with a very large predicate (close to the user size limit)",
    command: {find: collName, filter: createLargeObjectCloseToUserLimit(), limit: 1},
    expectedDiagnosticInfo: [
        ...commonDiagnostics,
        // Log the full command untruncated.
        '\'opDescription\': { find: \\"command_diagnostics\\", filter: { a: { $in: [ 1.0, 1.0, 1.0',
        'afterIn: \\"b\\"',
        ...defaultExpCtxLog,
        "'winningPlan': ",
        "'executionStats': ",
        omittedShardKeyLog,
    ],
});

const conn = MongoRunner.runMongod();
const {db, coll} = setup(conn, "test");
// Sanity check: commands larger than the internal size limit won't be accepted. Hence it's not
// possible that we would have to log them either.
assert.throwsWithCode(() => coll.findOne(createLargeObjectCloseToInternalLimit()), 17260);
MongoRunner.stopMongod(conn);
