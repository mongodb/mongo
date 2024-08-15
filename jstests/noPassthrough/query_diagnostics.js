/**
 * Test that tassert during find command execution will log diagnostics about the query.
 */

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");
const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(coll.insert({a: 1, b: 1}));

assert.commandWorked(db.adminCommand({
    'configureFailPoint': 'planExecutorAlwaysFails',
    'mode': 'alwaysOn',
    'data': {'tassert': true},
}));

const error = assert.throws(() => coll.findOne({a: 1, b: 1}));
assert.eq(error.code, 9028201, tojson(error));

assert.commandWorked(db.adminCommand({
    'configureFailPoint': 'planExecutorAlwaysFails',
    'mode': 'off',
}));

// Assert the diagnostics were logged
const logObj = assert.commandWorked(db.adminCommand({getLog: "global"}));
assert(logObj.log, "no log field");
assert.gt(logObj.log.length, 0, "no log lines");
assert(logObj.log.some(function(logLine) {
    return logLine.includes("ScopedDebugInfo") && logLine.includes("queryDiagnostics") &&
        // Original command included
        logLine.includes(
            "{'originalCommand': { find: \\\"query_diagnostics\\\", filter: { a: 1.0, b: 1.0 }, limit: 1.0");
}));

// We expect a non-zero exit code due to tassert triggered.
MongoRunner.stopMongod(conn, null, {allowedExitCode: MongoRunner.EXIT_ABRUPT});
