/**
 * Test that tassert during command execution for an unsharded collection in a sharded environment
 * will correctly show that the collection is unsharded.
 *
 * This test expects collections to persist across a restart.
 * @tags: [requires_persistence]
 */
import {
    assertOnDiagnosticLogContents,
    getQueryPlannerAlwaysFailsWithNamespace,
    runWithFailpoint
} from "jstests/libs/query/command_diagnostic_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = "test";
const collName = jsTestName();
const ns = dbName + "." + collName;
const omittedShardKeyLog = "omitted: collection isn't sharded";
const query = {
    a: 1,
    b: 1
};

const st = new ShardingTest({shards: [{useLogFiles: true}], mongos: 1});
const db = st.s.getDB(dbName);
const coll = db[collName];
coll.drop();
coll.insert({a: 1});

function runTest({description, command}) {
    const {failpointName, failpointOpts, errorCode} = getQueryPlannerAlwaysFailsWithNamespace(ns);

    const connAssert = st.rs0.getPrimary();
    jsTestLog("Running test case: " + tojson({description, command}));
    runWithFailpoint(connAssert.getDB(dbName), failpointName, failpointOpts, () => {
        assert.commandFailedWithCode(
            st.s.getDB(dbName).runCommand(command), errorCode, description);
    });

    assertOnDiagnosticLogContents({
        description: description,
        logFile: connAssert.fullOptions.logFile,
        expectedDiagnosticInfo: [omittedShardKeyLog]
    });

    // We expect a non-zero exit code due to tassert triggered. Restarting will also clear the
    // log files that we grep in assertOnDiagnosticLogContents for the next test case.
    st.rs0.restart(connAssert, {allowedExitCode: MongoRunner.EXIT_ABRUPT});
}

// Find
runTest({
    description: "find",
    command: {find: collName, filter: query, limit: 1},
});

// Aggregate
runTest({
    description: "aggregate",
    command: {aggregate: collName, pipeline: [{$match: query}], cursor: {}},
});

// FindAndModify
runTest({
    description: 'findAndModify remove',
    command: {
        findAndModify: collName,
        query: query,
        remove: true,
    },
});

// Explain
runTest({
    description: "explain find",
    command: {explain: {find: collName, filter: query, limit: 1}},
});

st.stop();
