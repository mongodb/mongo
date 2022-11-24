/**
 * Tests that $_internalOwningShard expression correctly computes the shard id the document belongs
 * to, while executing on mongod.
 *
 * @tags: [requires_fcv_63]
 */
(function() {
"use strict";

load("jstests/sharding/libs/create_sharded_collection_util.js");

const st = new ShardingTest({
    mongos: 1,
    config: 1,
    shards: 3,
});
const mongos = st.s;
const dbName = jsTestName();
const db = st.getDB(dbName);
const sourceColl = db["source"];
const destinationColl = db["destination"];

const shard0 = st.rs0;
const shard1 = st.rs1;
const shard2 = st.rs2;

// Retrieves the current shard version for the 'destinationColl' and returns the ShardVersion
// object.
function getCurrentShardVersion() {
    const shardVersionResult = assert.commandWorked(destinationColl.getShardVersion());
    return {
        v: shardVersionResult.version,
        e: shardVersionResult.versionEpoch,
        t: shardVersionResult.versionTimestamp,
    };
}

// Returns a projection stage with the $_internalOwningShard expression.
function buildProjectionStageWithOwningShardExpression(shardVersion) {
    return {
        $project: {
            _id: 0,
            shard: {
                $_internalOwningShard: {
                    shardKeyVal: {_id: "$_id"},
                    ns: destinationColl.getFullName(),
                    shardVersion: shardVersion,
                },
            },
            indexData: "$$ROOT",
        }
    };
}

// Asserts that $_internalOwningShard expression correctly computes the shard id.
function assertOwningShardExpressionResults(shardVersion, expectedResult) {
    const projectionStage = buildProjectionStageWithOwningShardExpression(shardVersion);
    assert.eq(sourceColl.aggregate([projectionStage, {$sort: {"indexData._id": 1}}]).toArray(),
              expectedResult);
}

// Asserts that $_internalOwningShard expression fails when routing information is stale.
function assertOwningShardExpressionFailure(shardVersion) {
    const projectionStage = buildProjectionStageWithOwningShardExpression(shardVersion);
    assert.commandFailedWithCode(
        db.runCommand({
            aggregate: sourceColl.getName(),
            pipeline: [projectionStage, {$sort: {"indexData._id": 1}}],
            cursor: {}
        }),
        [ErrorCodes.StaleConfig, ErrorCodes.ShardCannotRefreshDueToLocksHeld]);

    // Assert the expression fails while executing on the mongos.
    assert.commandFailedWithCode(db.runCommand({
        aggregate: sourceColl.getName(),
        pipeline: [{$sort: {_id: 1}}, projectionStage],
        cursor: {}
    }),
                                 6868600);
}

// Create a sharded source collection with the shard key on '_id' attribute and two chunks.
CreateShardedCollectionUtil.shardCollectionWithChunks(sourceColl, {_id: 1}, [
    {min: {_id: MinKey}, max: {_id: 50}, shard: st.shard2.shardName},
    {min: {_id: 50}, max: {_id: MaxKey}, shard: st.shard0.shardName},
]);

// Insert some data.
const documentOnShard0 = {
    _id: 1
};
const documentOnShard1 = {
    _id: 50
};
const documentOnShard2 = {
    _id: 100
};
assert.commandWorked(sourceColl.insert(documentOnShard0));
assert.commandWorked(sourceColl.insert(documentOnShard1));
assert.commandWorked(sourceColl.insert(documentOnShard2));

// Create a sharded destination collection with the shard key on '_id' attribute and three chunks.
CreateShardedCollectionUtil.shardCollectionWithChunks(destinationColl, {_id: 1}, [
    {min: {_id: MinKey}, max: {_id: 33}, shard: st.shard0.shardName},
    {min: {_id: 33}, max: {_id: 66}, shard: st.shard1.shardName},
    {min: {_id: 66}, max: {_id: MaxKey}, shard: st.shard2.shardName},
]);
const expectedResult = [
    {shard: `${dbName}-rs0`, indexData: documentOnShard0},
    {shard: `${dbName}-rs1`, indexData: documentOnShard1},
    {shard: `${dbName}-rs2`, indexData: documentOnShard2},
];

// Assert that every document belongs to a different shard.
const shardVersion = getCurrentShardVersion();
assertOwningShardExpressionResults(shardVersion, expectedResult);

// Flush the router config and assert that every document still belongs to the different shard.
[shard0, shard1, shard2].forEach(function(shard) {
    shard.nodes.forEach(function(node) {
        assert.commandWorked(node.adminCommand({flushRouterConfig: destinationColl.getFullName()}));
    });
});
assertOwningShardExpressionResults(shardVersion, expectedResult);

// Assert that $_internalOwningShard expression will fail when routing information is stale. This is
// simulated by providing a sharding version with a timestamp from the future.
const futureShardVersion =
    Object.assign({}, shardVersion, {t: new Timestamp(Math.pow(2, 32) - 1, 0)});
assertOwningShardExpressionFailure(futureShardVersion);

st.stop();
})();
