/*
 * Tests that _shardsvrCoordinateMultiUpdate runs on shard servers.
 * @tags: [
 *  featureFlagPauseMigrationsDuringMultiUpdatesAvailable,
 *  requires_fcv_72
 * ]
 */

const st = new ShardingTest({shards: {rs0: {nodes: 3}}});
const replicaSet = new ReplSetTest({nodes: 1});
replicaSet.startSet();
replicaSet.initiate();

const dbName = "test";
const collName = "coll";
const namespace = `${dbName}.${collName}`;

function assertCoordinateMultiUpdateReturns(connection, code) {
    const response =
        connection.adminCommand({_shardsvrCoordinateMultiUpdate: namespace, command: {}});
    if (code === ErrorCodes.OK) {
        assert.commandWorked(response);
    } else {
        assert.commandFailedWithCode(response, code)
    }
}

// Verify _shardsvrCoordinateMultiUpdate only runs on shard servers.
assertCoordinateMultiUpdateReturns(st.rs0.getPrimary(), ErrorCodes.OK);
assertCoordinateMultiUpdateReturns(st.rs0.getSecondary(), ErrorCodes.NotWritablePrimary);
assertCoordinateMultiUpdateReturns(st.s, ErrorCodes.CommandNotFound);
assertCoordinateMultiUpdateReturns(replicaSet.getPrimary(), ErrorCodes.NoShardingEnabled);

replicaSet.stopSet();
st.stop();
