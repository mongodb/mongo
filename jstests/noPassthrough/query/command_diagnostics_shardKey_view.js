/**
 * Test that tassert during a command against a view will log diagnostics
 * about the underlying collection being sharded or not.
 */
import {
    assertOnDiagnosticLogContents,
    getQueryPlannerAlwaysFailsWithNamespace,
    runWithFailpoint
} from "jstests/libs/query/command_diagnostic_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = "test";
const collName = "command_diagnostics";
const viewName = collName + "_view";
const ns = dbName + "." + collName;

// We want to set the failpoint on the underlying collection's namespace because we will hit the
// failpoint after we have resolved the view.
const {failpointName, failpointOpts, errorCode} = getQueryPlannerAlwaysFailsWithNamespace(ns);

const command = {
    aggregate: viewName,
    pipeline: [{$addFields: {b: 1}}],
    cursor: {}
};

function runTest(description, isSharded, connToRunCommand, connToCheckLogs) {
    runWithFailpoint(connToCheckLogs.getDB(dbName), failpointName, failpointOpts, () => {
        assert.commandFailedWithCode(
            connToRunCommand.getDB(dbName).runCommand(command),
            errorCode,
            "Expected pipeline against sharded view to fail with queryPlannerAlwaysFails failpoint");
    });

    const shardedDiagnostics = [
        `\'opDescription\': { aggregate: \\"${
                      collName}\\", pipeline: [ { $match: { a: 1.0 } }, { $addFields: { b: { $const: 1.0 } } } ]`,
        `\'shardKeyPattern\': { a: 1.0 }`
    ];
    const unshardedDiagnostics = [
        `resolvedPipeline: [ { $match: { a: 1.0 } } ] } ]`,
        `\'opDescription': { aggregate: \\"${viewName}\\", pipeline: [ { $addFields: { b:`,
        `omitted: collection isn't sharded`,
    ];
    assertOnDiagnosticLogContents({
        description: description,
        logFile: connToCheckLogs.fullOptions.logFile,
        expectedDiagnosticInfo: [
            `{\'currentOp\': { op: \\"command\\", ns: \\"${ns}\\"`,
            ...(isSharded ? shardedDiagnostics : unshardedDiagnostics),
        ]
    });
}

let description = "Testing tassert log diagnostics against unsharded view on standalone";
jsTestLog(description);
{
    const conn = MongoRunner.runMongod({useLogFiles: true});
    const db = conn.getDB(dbName);
    const coll = db[collName];
    coll.drop();
    coll.insert({a: 1});
    coll.insert({a: 2});

    assert.commandWorked(db.createView(viewName, coll.getName(), [{$match: {a: 1}}]));

    runTest(description, false /* isSharded */, conn, conn);

    // We expect a non-zero exit code due to tassert triggered.
    MongoRunner.stopMongod(conn, null, {allowedExitCode: MongoRunner.EXIT_ABRUPT});
}

description = "Testing tassert log diagnostics against sharded view";
jsTestLog(description);
{
    const shardKey = {a: 1};

    const st = new ShardingTest({shards: [{useLogFiles: true}], mongos: 1});
    const db = st.s.getDB(dbName);

    const coll = db[collName];
    coll.drop();
    coll.insert({a: 1});
    coll.insert({a: 2});
    assert.commandWorked(coll.createIndex(shardKey));
    assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: shardKey}));

    assert.commandWorked(db.createView(viewName, coll.getName(), [{$match: {a: 1}}]));

    runTest(description, true /* isSharded */, st.s, st.rs0.getPrimary());

    // We expect a non-zero exit code due to tassert triggered.
    st.stop({skipValidatingExitCode: true});
}

description = "Testing tassert log diagnostics against unsharded view in sharded environment";
jsTestLog(description);
{
    const st = new ShardingTest({shards: [{useLogFiles: true}], mongos: 1});
    const db = st.s.getDB(dbName);

    const coll = db[collName];
    coll.drop();
    coll.insert({a: 1});
    coll.insert({a: 2});

    assert.commandWorked(db.createView(viewName, coll.getName(), [{$match: {a: 1}}]));

    runTest(description, false /* isSharded */, st.s, st.rs0.getPrimary());

    // We expect a non-zero exit code due to tassert triggered.
    st.stop({skipValidatingExitCode: true});
}
