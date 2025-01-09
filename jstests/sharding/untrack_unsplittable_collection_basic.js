/**
 * Basic test for the untrackUnshardedCollection command.
 * @tags: [
 *   requires_fcv_80,
 *   # Requires a deterministic placement for the collection.
 *   assumes_balancer_off
 * ]
 */

function assertCollectionIsUntracked(st, primaryShard, ns, uuid) {
    // Make sure there is no entry on config.collections.
    assert.eq(0, st.s.getCollection('config.collections').countDocuments({_id: ns}));
    // Make sure there is no entry on config.chunks.
    assert.eq(0, st.s.getCollection('config.chunks').countDocuments({uuid: uuid}));

    // Make sure that persisted cached metadata was removed from the primary shard.
    const chunksCollName = 'cache.chunks.' + ns;
    const configDb = primaryShard.getDB("config");
    assert.eq(
        0,
        configDb.cache.collections.countDocuments({_id: ns}),
        "Found collection entry in 'config.cache.collections' after untrackUnshardedCollection for shard " +
            primaryShard.shardName);
    assert(!configDb[chunksCollName].exists());
}

// The test leaves orphan collections around. The metadata consistency is checked where required.
TestData.skipCheckMetadataConsistency = true;

const kDbName = 'db';
const kCollName = 'coll';
const kNss = kDbName + '.' + kCollName;

const st = new ShardingTest({mongos: 1, shards: 2, config: 1});
const shard0Name = st.shard0.shardName;
const shard1Name = st.shard1.shardName;
let lastUUID = null;

jsTest.log("Untrack a non tracked collection is a noop.");
{
    assert.commandWorked(st.s.adminCommand({enableSharding: kDbName, primaryShard: shard0Name}));
    assert.commandWorked(st.s.getCollection(kNss).insert({x: 1}));
    assert.commandWorked(st.s.adminCommand({untrackUnshardedCollection: kNss}));
}

jsTest.log("Untrack a collection on a non-primary shard returns error.");
{
    // Track and move the collection to shard 1.
    assert.commandWorked(st.s.adminCommand({moveCollection: kNss, toShard: shard1Name}));
    assert.commandFailedWithCode(st.s.adminCommand({untrackUnshardedCollection: kNss}),
                                 ErrorCodes.OperationFailed);
    lastUUID = st.s.getCollection('config.collections').findOne({_id: kNss}).uuid;
}

jsTest.log("Untrack a collection on the primary shard works.");
{
    assert.commandWorked(st.s.adminCommand({moveCollection: kNss, toShard: shard0Name}));
    assert.commandWorked(st.s.adminCommand({untrackUnshardedCollection: kNss}));
    assertCollectionIsUntracked(st, st.shard0, kNss, lastUUID);
}

jsTest.log("Untrack a collection on a new primary shard works and no orphans are left.");
{
    // Moving the primary will move the untracked collection.
    assert.commandWorked(st.s.adminCommand({movePrimary: kDbName, to: shard1Name}));

    // Move and track the collection again.
    assert.commandWorked(st.s.adminCommand({moveCollection: kNss, toShard: shard0Name}));
    lastUUID = st.s.getCollection('config.collections').findOne({_id: kNss}).uuid;

    // Moving the primary back to shard0 will allow to untrack the collection.
    assert.commandFailedWithCode(st.s.adminCommand({untrackUnshardedCollection: kNss}),
                                 ErrorCodes.OperationFailed);
    assert.commandWorked(st.s.adminCommand({movePrimary: kDbName, to: shard0Name}));

    assert.commandWorked(st.s.adminCommand({untrackUnshardedCollection: kNss}));
    assertCollectionIsUntracked(st, st.shard0, kNss, lastUUID);

    // Check no orphans are left.
    const inconsistencies = st.getDB(kDbName).checkMetadataConsistency().toArray();
    assert.eq(0, inconsistencies.length, tojson(inconsistencies));
}

jsTest.log("Untrack a collection on a new primary shard works but non-empty orphans are left.");
{
    // Moving the primary will move the untracked collection.
    assert.commandWorked(st.s.adminCommand({movePrimary: kDbName, to: shard1Name}));

    // Move and track the collection again.
    assert.commandWorked(st.s.adminCommand({moveCollection: kNss, toShard: shard0Name}));
    lastUUID = st.s.getCollection('config.collections').findOne({_id: kNss}).uuid;

    // Insert some data in the empty incarnation of the tracked collection in the primary. When a
    // collection is not empty, we expect the orphan collection not to be dropped by the untrack
    // command.
    st.rs1.getPrimary().getCollection(kNss).insert({x: 1});

    // Moving the primary back to shard0 will allow to untrack the collection.
    assert.commandFailedWithCode(st.s.adminCommand({untrackUnshardedCollection: kNss}),
                                 ErrorCodes.OperationFailed);
    assert.commandWorked(st.s.adminCommand({movePrimary: kDbName, to: shard0Name}));

    assert.commandWorked(st.s.adminCommand({untrackUnshardedCollection: kNss}));
    assertCollectionIsUntracked(st, st.shard0, kNss, lastUUID);

    // Check 1 orphan is left on the former primary.
    const inconsistencies = st.getDB(kDbName).checkMetadataConsistency().toArray();
    assert.eq(1, inconsistencies.length, tojson(inconsistencies));
    assert.eq("MisplacedCollection", inconsistencies[0].type, tojson(inconsistencies[0]));
}
st.stop();
