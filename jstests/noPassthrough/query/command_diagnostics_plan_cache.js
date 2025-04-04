/**
 * Test that tassert during plan cache solution to QuerySolution transformation will log diagnostics
 * about the plan retrieved from the cache.
 */
import {
    assertOnDiagnosticLogContents,
    planFromCacheAlwaysFails,
    runWithFailpoint
} from "jstests/libs/query/command_diagnostic_utils.js";
import {
    checkSbeFullyEnabled,
} from "jstests/libs/query/sbe_util.js";

// This test triggers an unclean shutdown, which may cause inaccurate fast counts.
TestData.skipEnforceFastCountOnValidate = true;

const dbName = "test";
const collName = "command_diagnostics";

const {failpointName, failpointOpts, errorCode} = planFromCacheAlwaysFails;

function runTest(description, command, diagnosticInfo) {
    const conn = MongoRunner.runMongod({useLogFiles: true});
    const db = conn.getDB(dbName);

    if (checkSbeFullyEnabled(db)) {
        MongoRunner.stopMongod(conn);
        jsTestLog("Skipping command_diagnostics_plan_cache.js because SBE is fully enabled " +
                  "and the plan cache diagnostic logging is not enabled for the SBE plan cache.");
        quit();
    }

    const coll = db[collName];
    coll.drop();
    coll.insert({a: 1});

    // Create two indexes so that we multiplan & cache a plan.
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({a: 1, b: 1}));

    // Run the command twice to put the plan in the cache and activate it.
    assert.commandWorked(conn.getDB(dbName).runCommand(command));
    assert.commandWorked(conn.getDB(dbName).runCommand(command));

    // Run the command a third time to use the plan from the cache.
    runWithFailpoint(conn.getDB(dbName), failpointName, failpointOpts, () => {
        assert.commandFailedWithCode(
            conn.getDB(dbName).runCommand(command),
            errorCode,
            "Expected query to fail with planFromCacheAlwaysFails failpoint");
    });

    assertOnDiagnosticLogContents({
        description: description,
        logFile: conn.fullOptions.logFile,
        expectedDiagnosticInfo: diagnosticInfo
    });

    // We expect a non-zero exit code due to tassert triggered.
    MongoRunner.stopMongod(conn, null, {allowedExitCode: MongoRunner.EXIT_ABRUPT});
}

runTest("classic execution", {find: collName, filter: {a: {$lt: 5}}}, [
    `opDescription': { find: \\"${collName}\\", filter: { a: { $lt: 5.0 } }`,
    `queryPlannerParams: {mainCollectionInfo: {exists: true, isTimeseries: false, indexes: [`,
    `name: \'(_id_, )\'`,
    `name: '(a_1, )`,
    `name: '(a_1_b_1, )`,
    `planCacheSolution': (index-tagged expression tree:`,
]);

runTest(
    "SBE execution",
    {
        aggregate: collName,
        pipeline: [{$match: {a: {$lt: 5}}}, {$group: {_id: null, s: {$sum: "$a"}}}],
        cursor: {}
    },
    [
        `opDescription': { aggregate: \\"${
            collName}\\", pipeline: [ { $match: { a: { $lt: 5.0 } } }, { $group: { _id: null, s: { $sum: \\"$a\\" } } } ]`,
        `queryPlannerParams: {mainCollectionInfo: {exists: true, isTimeseries: false, indexes: [`,
        `name: \'(_id_, )\'`,
        `name: '(a_1, )`,
        `name: '(a_1_b_1, )`,
        `planCacheSolution': (index-tagged expression tree:`,
    ]);
