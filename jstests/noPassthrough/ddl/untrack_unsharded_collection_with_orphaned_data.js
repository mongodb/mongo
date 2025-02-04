/*
 * Test covering the expected behavior of untrackCollection against the existence an orphaned
 * collection in the local catalog of one or more shards.
 * @tags: [
 *   requires_fcv_81,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

// The test injects a metadata inconsistency.
TestData.skipCheckMetadataConsistency = true;

const st = new ShardingTest({mongos: 1, shards: 2, config: 1});
const shard0Name = st.shard0.shardName;
const shard1Name = st.shard1.shardName;
let lastUUID = null;

const kDbName = 'db';
const kCollName = 'coll';
const kNss = kDbName + '.' + kCollName;

function verifyShardingCatalogStateAfterUntracking(st, primaryShard, ns, uuid) {
    // Make sure there is no entry on config.collections.
    assert.eq(0, st.s.getCollection('config.collections').countDocuments({_id: ns}));
    // Make sure there is no entry on config.chunks.
    assert.eq(0, st.s.getCollection('config.chunks').countDocuments({uuid: uuid}));

    // Make sure that the primary shard refreshed its filtering metadata upon completing the
    // command, so that there is no document on the related collections.
    const chunksCollName = 'cache.chunks.' + ns;
    const configDb = primaryShard.getDB("config");
    assert.eq(
        0,
        configDb.cache.collections.countDocuments({_id: ns}),
        "Found collection entry in 'config.cache.collections' after untrackUnshardedCollection for shard " +
            primaryShard.shardName);
    assert(!configDb[chunksCollName].exists());
}

jsTest.log("Untrack a collection on a new primary shard works but non-empty orphans are left.");
{
    // Populate an untracked collection.
    assert.commandWorked(st.s.adminCommand({enableSharding: kDbName, primaryShard: shard0Name}));
    assert.commandWorked(st.s.getCollection(kNss).insert({x: 1}));

    // Moving the primary of the parent DB will move the untracked collection.
    assert.commandWorked(st.s.adminCommand({movePrimary: kDbName, to: shard1Name}));

    // Use moveCollection to place the (now tracked) collection outside the current primary shard.
    assert.commandWorked(st.s.adminCommand({moveCollection: kNss, toShard: shard0Name}));
    lastUUID = st.s.getCollection('config.collections').findOne({_id: kNss}).uuid;

    // Inject an orphaned document for the collection in the primary shard;
    // The upcoming untrackCollection command is expected to leave it untouched (since the
    // collection is not empty).
    st.rs1.getPrimary().getCollection(kNss).insert({x: 1});

    // Untrack the collection; the operation is only expected to succeed when its data are located
    // on the primary shard.
    assert.commandFailedWithCode(st.s.adminCommand({untrackUnshardedCollection: kNss}),
                                 ErrorCodes.OperationFailed);
    assert.commandWorked(st.s.adminCommand({movePrimary: kDbName, to: shard0Name}));
    assert.commandWorked(st.s.adminCommand({untrackUnshardedCollection: kNss}));
    verifyShardingCatalogStateAfterUntracking(st, st.shard0, kNss, lastUUID);

    // Check that the orphan document is still present on the former primary.
    const inconsistencies = st.getDB(kDbName).checkMetadataConsistency().toArray();
    assert.eq(1, inconsistencies.length, tojson(inconsistencies));
    assert.eq("MisplacedCollection", inconsistencies[0].type, tojson(inconsistencies[0]));
}
st.stop();
