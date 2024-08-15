/**
 * Test that tassert during find command execution will log diagnostics about the query.
 */

const hasEnterpriseModule = getBuildInfo().modules.includes("enterprise");

function setup(conn) {
    const db = conn.getDB("test");
    const coll = db[jsTestName()];
    coll.drop();
    assert.commandWorked(coll.insert({a: 1, b: 1}));
    return {db, coll};
}

function runTest({description, filter, expectedDiagnosticInfo, redact}) {
    // Can't run cases that depend on log redaction without the enterprise module.
    if (!hasEnterpriseModule && redact) {
        return;
    }

    // Each test case needs to start a new mongod to clear the previous logs.
    const conn = MongoRunner.runMongod({useLogFiles: true});
    const {db, coll} = setup(conn);

    if (hasEnterpriseModule) {
        assert.commandWorked(db.adminCommand({setParameter: 1, redactClientLogData: redact}));
    }

    assert.commandWorked(db.adminCommand({
        'configureFailPoint': 'planExecutorAlwaysFails',
        'mode': 'alwaysOn',
        'data': {'tassert': true},
    }));

    const error = assert.throws(() => coll.findOne(filter));
    assert.eq(error.code, 9028201, tojson(error));

    assert.commandWorked(db.adminCommand({
        'configureFailPoint': 'planExecutorAlwaysFails',
        'mode': 'off',
    }));

    // By default, the log will not be printed to stdout if 'useLogFiles' is enabled.
    const log = cat(conn.fullOptions.logFile);
    print("Log file contents", log);

    // Assert the diagnostics were logged.
    const logLines = log.split("\n");
    assert.gt(logLines.length, 0, "no log lines");

    const queryDiagnostics = logLines.find(function(logLine) {
        return logLine.includes("ScopedDebugInfo") && logLine.includes("queryDiagnostics");
    });
    assert(queryDiagnostics, "no log line containing query diagnostics");

    for (const diagnosticInfo of expectedDiagnosticInfo) {
        assert(queryDiagnostics.includes(diagnosticInfo),
               `${description}: missing '${diagnosticInfo}'`);
    }

    // We expect a non-zero exit code due to tassert triggered.
    MongoRunner.stopMongod(conn, null, {allowedExitCode: MongoRunner.EXIT_ABRUPT});
}

const commonDiagnosticInfo = [
    `{\'currentOp\': { op: \\"query\\", ns: \\"test.${jsTestName()}\\"`,
    "queryHash:",
    "planCacheKey:",
    "queryShapeHash:",
    'queryFramework:',
    'planSummary: \\"COLLSCAN\\"',
    'protocol: \\"op_msg\\"',
    "millis:",
    "planningTimeMicros:",
    // We specifically don't want to log any information that requires synchronization or disk
    // access.
    "locks: {}",
    "flowControl: {}",
];

runTest({
    description: "simple find",
    filter: {a: 1, b: 1},
    expectedDiagnosticInfo: [
        ...commonDiagnosticInfo,
        // Original command included.
        '\'opDescription\': { find: \\"query_diagnostics\\", filter: { a: 1.0, b: 1.0 }, limit: 1.0, singleBatch: true',
    ]
});

runTest({
    description: "simple find with log redaction",
    filter: {a: 1, b: 1},
    redact: true,
    expectedDiagnosticInfo: [
        '{\'currentOp\': { op: \\"###\\", ns: \\"###\\"',
        '\'opDescription\': { find: \\"###\\", filter: { a: \\"###\\", b: \\"###\\" }, limit: \\"###\\"',
        'planSummary: \\"###\\"',
    ],
});

// Test that large commands are handled properly.
const bsonMaxUserSize = 16 * 1024 * 1024;
const bsonMaxInternalSize = bsonMaxUserSize + (16 * 1024);

function createLargeObject({inListLength}) {
    // Use a large $in-list, since BSONObj.toString() can automatically truncate strings.
    return {a: {$in: [...Array(inListLength).fill(1)]}, afterIn: "b"};
}

function createLargeObjectCloseToUserLimit() {
    // Internally, BSON arrays are represented as objects (e.g. {'0': val, '1': val, ..., '1000000':
    // val}), which makes coming up with formula to calculate the desired length a bit tedious. The
    // in-list length is chosen such that the size of resulting predicate is as close to the max
    // size as possible.
    const result = createLargeObject({inListLength: (1024 * 1024) + 3676});
    const remainingBytes = bsonMaxUserSize - Object.bsonsize(result);
    assert.eq(remainingBytes, 4);
    return result;
}

function createLargeObjectCloseToInternalLimit() {
    // The in-list length is chosen such that the size of resulting predicate is as close to the max
    // size as possible. See the comment in the above function for further explanation.
    const result = createLargeObject({inListLength: (1024 * 1024) + 4639});
    const remainingBytes = bsonMaxInternalSize - Object.bsonsize(result);
    assert.eq(remainingBytes, 17);
    return result;
}

runTest({
    description: "find with a very large predicate (close to the user size limit)",
    filter: createLargeObjectCloseToUserLimit(),
    expectedDiagnosticInfo: [
        ...commonDiagnosticInfo,
        // Log the full command untruncated.
        '\'opDescription\': { find: \\"query_diagnostics\\", filter: { a: { $in: [ 1.0, 1.0, 1.0',
        'afterIn: \\"b\\"',
    ],
});

const conn = MongoRunner.runMongod();
const {db, coll} = setup(conn);
// Sanity check: commands larger than the internal size limit won't be accepted. Hence it's not
// possible that we would have to log them either.
assert.throwsWithCode(() => coll.findOne(createLargeObjectCloseToInternalLimit()), 17260);
MongoRunner.stopMongod(conn);
