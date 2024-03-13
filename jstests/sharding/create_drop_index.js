/*
 * Tests create/drop index for all combinations of sharded/unsplittable collections initially
 * created on a db's primary/non-primary shard.
 *
 * @tags: [
 *   # Needed to run createUnsplittableCollection
 *   featureFlagAuthoritativeShardCollection,
 * ]
 *
 */

const st = new ShardingTest({shards: 2, nodes: 1, config: 1});
const mongos = st.s;

const primaryShardName = st.shard0.shardName;
const nonPrimaryShardName = st.shard1.shardName;
const primaryShardConn = st.rs0.getPrimary();
const nonPrimaryShardConn = st.rs1.getPrimary();

const dbName = 'test';
const db = mongos.getDB(dbName);

assert.commandWorked(mongos.adminCommand({enableSharding: dbName, primaryShard: primaryShardName}));

function getIndexes(conn, collectionName) {
    return assert.commandWorked(conn.getDB(dbName).runCommand({listIndexes: collectionName}))
        .cursor.firstBatch;
}

function testCreateDropIndexes(collMustBeCreatedOnPrimaryShard, collMustBeSharded) {
    const collName = (collMustBeSharded ? '' : 'un') + 'shardedCollection' +
        (collMustBeCreatedOnPrimaryShard ? '' : 'Not') + 'OnPrimary';
    const ns = dbName + '.' + collName;

    if (!collMustBeCreatedOnPrimaryShard) {
        assert.commandWorked(mongos.getDB(dbName).runCommand(
            {createUnsplittableCollection: collName, dataShard: nonPrimaryShardName}));
    }

    const coll = db[collName];

    if (collMustBeSharded) {
        // Shard/distribute collection: shard0 will own [minKey, 0) and shard1 will own [0, maxKey)
        assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {_id: 1}}));
        assert.commandWorked(st.splitAt(ns, {_id: 0}));
        assert.commandWorked(
            mongos.adminCommand({moveChunk: ns, find: {_id: -1}, to: primaryShardName}));
        assert.commandWorked(
            mongos.adminCommand({moveChunk: ns, find: {_id: 0}, to: nonPrimaryShardName}));
    }

    // Create index and check that the expected shards have it (indexes: {_id: 1} and {a:1})
    const indexSpec = {a: 1};
    assert.commandWorked(coll.createIndex(indexSpec));

    if (collMustBeSharded) {
        const indexesOnPrimaryShard = getIndexes(primaryShardConn, collName);
        const indexesOnSecondaryShard = getIndexes(nonPrimaryShardConn, collName);
        assert.eq(indexesOnPrimaryShard, indexesOnSecondaryShard);
        assert.eq(indexesOnPrimaryShard.length, 2);
    } else {
        const dataShard = collMustBeCreatedOnPrimaryShard ? primaryShardConn : nonPrimaryShardConn;
        const indexesOnDataShard = getIndexes(dataShard, collName);
        assert.eq(indexesOnDataShard.length, 2);
    }

    // Drop index and double check that the shards do not have it (indexes: {_id:1})
    assert.commandWorked(db.runCommand({dropIndexes: coll.getName(), index: indexSpec}));

    if (collMustBeSharded) {
        const indexesOnPrimaryShard = getIndexes(primaryShardConn, collName);
        const indexesOnSecondaryShard = getIndexes(nonPrimaryShardConn, collName);
        assert.eq(indexesOnPrimaryShard, indexesOnSecondaryShard);
        assert.eq(indexesOnPrimaryShard.length, 1);
    } else {
        const dataShard = collMustBeCreatedOnPrimaryShard ? primaryShardConn : nonPrimaryShardConn;
        const indexesOnDataShard = getIndexes(dataShard, collName);
        assert.eq(indexesOnDataShard.length, 1);
    }
}

testCreateDropIndexes(true /* collMustBeCreatedOnPrimaryShard */, false /* collMustBeSharded */);
testCreateDropIndexes(false /* collMustBeCreatedOnPrimaryShard */, true /* collMustBeSharded */);
testCreateDropIndexes(true /* collMustBeCreatedOnPrimaryShard */, true /* collMustBeSharded */);
testCreateDropIndexes(false /* collMustBeCreatedOnPrimaryShard */, false /* collMustBeSharded */);

st.stop();
