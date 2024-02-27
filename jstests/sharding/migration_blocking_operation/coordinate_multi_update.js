/*
 * Basic tests for _shardsvrCoordinateMultiUpdate.
 * @tags: [
 *  featureFlagPauseMigrationsDuringMultiUpdatesAvailable,
 *  requires_fcv_80
 * ]
 */

const st = new ShardingTest({shards: {rs0: {nodes: 3}}});
const replicaSet = new ReplSetTest({nodes: 1});
replicaSet.startSet();
replicaSet.initiate();

const dbName = "test";
const collName = "coll";
const namespace = `${dbName}.${collName}`;

assert.commandWorked(st.s0.getDB(dbName).getCollection(collName).insertMany([
    {
        _id: 1,
        member: "abc123",
        points: 0,
    },
    {
        _id: 2,
        member: "abc123",
        points: 59,
    },
]));

function assertCoordinateMultiUpdateReturns(connection, code) {
    const response = connection.adminCommand({
        _shardsvrCoordinateMultiUpdate: namespace,
        uuid: UUID(),
        command: {
            update: collName,
            updates: [{q: {member: "abc123"}, u: {$set: {points: 50}}, multi: true}]
        }
    });
    if (code === ErrorCodes.OK) {
        const res = assert.commandWorked(response);
        const underlyingUpdateResult = res["result"];
        assert.eq(underlyingUpdateResult["nModified"], 2);
        assert.eq(underlyingUpdateResult["n"], 2);
        assert.eq(underlyingUpdateResult["ok"], 1);
    } else {
        assert.commandFailedWithCode(response, code)
    }
}

// Command runs successfully on shard server.
assertCoordinateMultiUpdateReturns(st.rs0.getPrimary(), ErrorCodes.OK);
// Verify _shardsvrCoordinateMultiUpdate only runs on shard servers.
assertCoordinateMultiUpdateReturns(st.rs0.getSecondary(), ErrorCodes.NotWritablePrimary);
assertCoordinateMultiUpdateReturns(st.s, ErrorCodes.CommandNotFound);
assertCoordinateMultiUpdateReturns(
    replicaSet.getPrimary(),
    [ErrorCodes.ShardingStateNotInitialized, ErrorCodes.NoShardingEnabled_OBSOLETE]);

replicaSet.stopSet();
st.stop();
