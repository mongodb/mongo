/**
 * Test that running resharding in relaxed mode works when
 * the local catalogs and the sharding catalog do not agree
 * on the collection's UUID. Ensures that after running resharding
 * in relaxed mode fixes the collection UUID mismatch.
 *
 * @tags: [
 *  requires_sharding,
 *  requires_fcv_81,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {getUUIDFromConfigCollections, getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";

const st = new ShardingTest({
    mongos: 1,
    config: 1,
    shards: 2,
    rs: {nodes: 2},
});

const dbName = 'test';
const collName = 'coll';
const nss = dbName + "." + collName;
const db = st.getDB(dbName);
const coll = db.getCollection(collName);
const shardKey = {
    key: 1
};

// shard collection such that shard0 is the primary and shard0 and shard1 both own some documents
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
CreateShardedCollectionUtil.shardCollectionWithChunks(coll, shardKey, [
    {min: {key: MinKey}, max: {key: 5}, shard: st.shard0.shardName},
    {min: {key: 5}, max: {key: MaxKey}, shard: st.shard1.shardName},
]);
var bulk = coll.initializeUnorderedBulkOp();
for (var i = 0; i < 10; i++) {
    bulk.insert({key: i});
}
assert.commandWorked(bulk.execute({w: "majority"}));

// drop and recreate the collection directly on shard1 so that it has a different collection UUID
st.shard1.getCollection(nss).drop();
var bulk = st.shard1.getCollection(nss).initializeUnorderedBulkOp();
for (var i = 5; i < 10; i++) {
    bulk.insert({key: i});
}
assert.commandWorked(bulk.execute({w: "majority"}));

// Assert that shard0's view of the collection UUID matches the view from config.collection and
// that shard1's view of the collection UUID matches if and only if shard1ShouldMatch
function checkShardUUIDsAgainstConfig(shard1ShouldMatch) {
    let configCollectionUUID = getUUIDFromConfigCollections(st.s, nss);
    let shard0CollectionUUID = getUUIDFromListCollections(st.shard0.getDB(dbName), collName);
    let shard1CollectionUUID = getUUIDFromListCollections(st.shard1.getDB(dbName), collName);
    assert.eq(shard0CollectionUUID, configCollectionUUID);
    if (shard1ShouldMatch) {
        assert.eq(shard1CollectionUUID, configCollectionUUID);
    } else {
        assert.neq(shard1CollectionUUID, configCollectionUUID);
    }
}

// Run the resharding command with the same shard key and distribution. Include
// the relaxed parameter if it is defined
function reshardingCommandWithRelaxed(relaxed) {
    const cmd = {
        reshardCollection: nss,
        key: shardKey,
        forceRedistribution: true,
        shardDistribution: [
            {min: {key: MinKey}, max: {key: 5}, shard: st.shard0.shardName},
            {min: {key: 5}, max: {key: MaxKey}, shard: st.shard1.shardName},
        ],
    };
    if (relaxed !== undefined) {
        cmd.relaxed = relaxed;
    }
    return st.s.adminCommand(cmd);
}

// assert that shard0's collection UUID matches config.collections and shard1's doesn't
checkShardUUIDsAgainstConfig(false);

// assert that running reshard command without relaxed mode fails due to a CollectionUUIDMismatch
assert.commandFailedWithCode(reshardingCommandWithRelaxed(undefined),
                             ErrorCodes.CollectionUUIDMismatch);

// assert that the collection UUID is still mistmached
checkShardUUIDsAgainstConfig(false);

// assert that running reshard command with relaxed mode = false fails as well
assert.commandFailedWithCode(reshardingCommandWithRelaxed(false),
                             ErrorCodes.CollectionUUIDMismatch);

// assert that the collection UUID is still mistmached
checkShardUUIDsAgainstConfig(false);

// assert that running reshard command with relaxed = true succeeds
assert.commandWorked(reshardingCommandWithRelaxed(true));

// assert that the collection UUIDs all match now
checkShardUUIDsAgainstConfig(true);

st.stop();
