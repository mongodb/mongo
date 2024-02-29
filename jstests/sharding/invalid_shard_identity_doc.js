/**
 * Tests that a shard identity document cannot be updated with a shardName that is not
 * allowed for the server's cluster role. Invalid shard identity document inserts are covered by
 * mixed_cluster_roles.js.
 *
 * @tags: [
 *   requires_fcv_70,
 *   featureFlagTransitionToCatalogShard,
 * ]
 */
const st = new ShardingTest({shards: 2, configShard: true});

var configConnStr = st.configRS.getURL();

var shardIdentityDoc = {
    _id: "shardIdentity",
    configsvrConnectionString: configConnStr,
    shardName: "config",
    clusterId: ObjectId()
};

// Update with shard name "config" on shard server should fail
let res = assert.commandFailedWithCode(
    st.shard1.getDB('admin').runCommand(
        {update: "system.version", updates: [{q: {"_id": "shardIdentity"}, u: shardIdentityDoc}]}),
    ErrorCodes.UnsupportedFormat);
assert.eq(
    res.writeErrors[0].errmsg,
    "Invalid shard identity document: the shard name for a shard server cannot be \"config\"");

// Update with shard name "pizza" on config server should fail
shardIdentityDoc.shardName = "pizza";
res = assert.commandFailedWithCode(
    st.shard0.getDB('admin').runCommand(
        {update: "system.version", updates: [{q: {"_id": "shardIdentity"}, u: shardIdentityDoc}]}),
    ErrorCodes.UnsupportedFormat);
assert.eq(
    res.writeErrors[0].errmsg,
    "Invalid shard identity document: the shard name for a config server cannot be \"pizza\"");

// TODO SERVER-74570: Enable parallel shutdown
st.stop({parallelSupported: false});