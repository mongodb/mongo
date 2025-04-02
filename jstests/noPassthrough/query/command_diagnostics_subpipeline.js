/**
 * Test that tassert during subpipeline execution against a sharded collection will log diagnostics
 * about the collection being sharded.
 */
import {
    assertOnDiagnosticLogContents,
    getQueryPlannerAlwaysFailsWithNamespace,
    runWithFailpoint
} from "jstests/libs/query/command_diagnostic_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// This test triggers an unclean shutdown, which may cause inaccurate fast counts.
TestData.skipEnforceFastCountOnValidate = true;

const dbName = "test";
const localCollName = jsTestName();
const shardedForeignCollName = "shardedForeignColl";
const foreignNs = dbName + "." + shardedForeignCollName;
const localShardKey = {
    a: 1
};
const foreignShardKey = {
    b: 1
};

const st = new ShardingTest({shards: [{}, {useLogFiles: true}], mongos: 1});
const db = st.s.getDB(dbName);

// Shard0 has the local collection with one doc.
const localColl = db[localCollName];
localColl.insert({a: 1});
assert.commandWorked(localColl.createIndex(localShardKey));
assert.commandWorked(
    st.s.adminCommand({shardCollection: localColl.getFullName(), key: localShardKey}));
assert.commandWorked(
    db.adminCommand({moveChunk: localColl.getFullName(), find: {a: 1}, to: st.shard0.shardName}));

// Shard1 has the foreign collection with one doc.
const shardedForeignColl = db[shardedForeignCollName];
shardedForeignColl.insert({b: 1});
assert.commandWorked(shardedForeignColl.createIndex(foreignShardKey));
assert.commandWorked(
    st.s.adminCommand({shardCollection: shardedForeignColl.getFullName(), key: foreignShardKey}));
assert.commandWorked(db.adminCommand(
    {moveChunk: shardedForeignColl.getFullName(), find: {b: 1}, to: st.shard1.shardName}));

const {failpointName, failpointOpts, errorCode} =
    getQueryPlannerAlwaysFailsWithNamespace(foreignNs);

const command = {
    aggregate: localCollName,
    pipeline: [{
        $lookup: {
            from: shardedForeignCollName,
            pipeline: [{$match: {$expr: {$eq: ["$b", 1]}}}],
            as: "results",
        }
    }],
    cursor: {}
};

// Set the failpoint on Shard1. During the $lookup, we will run the subpipeline on Shard1 and hit
// the failpoint.
runWithFailpoint(st.rs1.getPrimary().getDB(dbName), failpointName, failpointOpts, () => {
    assert.commandFailedWithCode(
        st.s.getDB(dbName).runCommand(command),
        errorCode,
        "Expected $lookup to fail with the queryPlannerAlwaysFails failpoint");
});
assertOnDiagnosticLogContents({
    description: "sharded $lookup with subpipeline",
    logFile: st.rs1.getPrimary().fullOptions.logFile,
    expectedDiagnosticInfo: [
        `{\'currentOp\': { op: \\"command\\", ns: \\"${foreignNs}\\"`,
        `\'opDescription\': { aggregate: \\"shardedForeignColl\\", pipeline: [ { $match: { $expr: { $eq: [ \\"$b\\", 1.0 ] } } } ]`,
        `\'shardKeyPattern\': { b: 1.0 }`,
    ]
});

// We expect a non-zero exit code due to tassert triggered.
st.stop({skipValidatingExitCode: true});
