/**
 * Tests that unrecoverable errors during resharding's collection cloning are handled by the
 * recipient.
 *
 * @tags: [
 *   requires_fcv_49,
 *   uses_atclustertime,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/parallelTester.js");
load("jstests/sharding/libs/create_sharded_collection_util.js");

const st = new ShardingTest({
    mongos: 1,
    config: 1,
    shards: 2,
    rs: {nodes: 1},
    rsOptions: {
        setParameter: {
            "failpoint.WTPreserveSnapshotHistoryIndefinitely": tojson({mode: "alwaysOn"}),
        }
    }
});

const inputCollection = st.s.getCollection("reshardingDb.coll");

CreateShardedCollectionUtil.shardCollectionWithChunks(inputCollection, {oldKey: 1}, [
    {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: st.shard0.shardName},
    {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: st.shard1.shardName},
]);

// The following documents violate the global _id uniqueness assumption of sharded collections. It
// is possible to construct such a sharded collection due to how each shard independently enforces
// the uniqueness of _id values for only the documents it owns. The resharding operation is expected
// to abort upon discovering this violation.
assert.commandWorked(inputCollection.insert(
    [
        {_id: 0, info: "stays on shard0", oldKey: -10, newKey: -10},
        {_id: 0, info: "moves to shard0", oldKey: 10, newKey: -10},
        {_id: 1, info: "moves to shard1", oldKey: -10, newKey: 10},
        {_id: 1, info: "stays on shard1", oldKey: 10, newKey: 10},
    ],
    {writeConcern: {w: "majority"}}));

// In the current implementation, the _configsvrReshardCollection command won't ever complete if one
// of the donor or recipient shards encounters an unrecoverable error. To work around this
// limitation, we verify the recipient shard transitioned itself into the "error" state as a result
// of the duplicate key error during resharding's collection cloning.
//
// TODO SERVER-50584: Remove the separate thread from this test and instead directly assert that the
// reshardCollection command fails with an error.
//
// We use a special appName to identify the _configsvrReshardCollection command because the command
// is too large and would otherwise be truncated by the currentOp() output.
const kAppName = "testReshardCollectionThread";
const thread = new Thread(function(host, appName, commandObj) {
    const conn = new Mongo(`mongodb://${host}/?appName=${appName}`);
    assert.commandFailedWithCode(conn.adminCommand(commandObj), ErrorCodes.Interrupted);
}, st.s.host, kAppName, {
    reshardCollection: inputCollection.getFullName(),
    key: {newKey: 1},
    _presetReshardedChunks: [
        {min: {newKey: MinKey}, max: {newKey: 0}, recipientShardId: st.shard0.shardName},
        {min: {newKey: 0}, max: {newKey: MaxKey}, recipientShardId: st.shard1.shardName},
    ],
});

thread.start();

function assertEventuallyErrorsLocally(shard) {
    const recipientCollection =
        shard.rs.getPrimary().getCollection("config.localReshardingOperations.recipient");

    assert.soon(
        () => {
            return recipientCollection.findOne({state: "error"}) !== null;
        },
        () => {
            return "recipient shard " + shard.shardName +
                " never transitioned to the error state: " + tojson(recipientCollection.findOne());
        });
}

assertEventuallyErrorsLocally(st.shard0);
assertEventuallyErrorsLocally(st.shard1);

const configPrimary = st.configRS.getPrimary().getDB("config");
const ops = assert.commandWorked(configPrimary.currentOp({appName: kAppName}));
assert.eq(1, ops.inprog.length, () => {
    return "failed to find _configsvrReshardCollection command in: " +
        tojson(configPrimary.currentOp());
});

assert.commandWorked(configPrimary.killOp(ops.inprog[0].opid));
thread.join();

st.stop();
})();
