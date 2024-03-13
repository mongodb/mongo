/*
 * Test the collmod command against unsplittable collections.
 * @tags: [
 *   # Needed to run createUnsplittableCollection
 *   featureFlagAuthoritativeShardCollection,
 * ]
 */

function assertIndexExists(coll, indexKey, options, connections) {
    connections.forEach((conn) => {
        const db = conn.getDB(coll.getDB().getName());
        const res = db.runCommand({listIndexes: coll.getName()});
        assert.commandWorked(res);
        const indexes = new DBCommandCursor(db, res).toArray();

        const expectedIndex = indexes.filter(function(idx) {
            // Checkk that index key match
            if (!friendlyEqual(idx.key, indexKey)) {
                return false;
            }

            // Check that all options match
            for (const optName in options) {
                if (idx[optName] !== options[optName]) {
                    return false;
                }
            }
            return true;
        });

        assert.eq(1, expectedIndex.length, "Index not found on " + conn.name);
    })
}

function assertIndexDoesntExist(coll, indexKey, options, connections) {
    assert.throws(() => {
        assertIndexExists(coll, indexKey, options, connections);
    })
}

let collId = 1;
function createUnsplittableCollection(db, opts) {
    const collName = "coll" + collId++;

    var options = opts || {};
    var cmd = {createUnsplittableCollection: collName};
    Object.extend(cmd, options);

    assert.commandWorked(db.runCommand(cmd));

    return db[collName];
}

// Test setup
const st = new ShardingTest({shards: 3});
const mongos = st.s;
const db = mongos.getDB(jsTestName());
const configDb = mongos.getDB("config");
const shard0_ps = st.shard0;
const shard1 = st.shard1;
const shard2 = st.shard2;
const kIndexKey = {
    key: 1
};

// Ensure the db primary is shard0.
st.s.adminCommand({enableSharding: db.getName(), primaryShard: shard0_ps.shardName});

jsTest.log("Test collmod over an unsplittable collection living on the DBPrimary shard.");
{
    let coll = createUnsplittableCollection(db, {});
    assert.commandWorked(coll.createIndex(kIndexKey, {expireAfterSeconds: 3333}));

    // Index must exist on the primaryShard but not on the other shards since the collection is only
    // present on the primary shard.
    assertIndexExists(coll, kIndexKey, {expireAfterSeconds: 3333}, [mongos, shard0_ps]);
    assertIndexDoesntExist(coll, kIndexKey, {expireAfterSeconds: 3333}, [shard1, shard2]);

    // Update index options calling collmod
    const res = assert.commandWorked(db.runCommand(
        {collMod: coll.getName(), index: {keyPattern: kIndexKey, expireAfterSeconds: 1111}}));

    assertIndexExists(coll, kIndexKey, {expireAfterSeconds: 1111}, [mongos, shard0_ps]);
    assertIndexDoesntExist(coll, kIndexKey, {expireAfterSeconds: 1111}, [shard1, shard2]);
}

jsTest.log("Test collmod over an unsplittable collection living outside the DBPrimary shard.");
{
    let coll = createUnsplittableCollection(db, {dataShard: shard1.shardName});
    assert.commandWorked(coll.createIndex(kIndexKey, {expireAfterSeconds: 3333}));

    // Index must exist on the shard where the collection is living and on the primaryShard.
    assertIndexExists(coll, kIndexKey, {expireAfterSeconds: 3333}, [mongos, shard1]);
    assertIndexDoesntExist(coll, kIndexKey, {expireAfterSeconds: 3333}, [shard0_ps, shard2]);

    // Update index options calling collmod
    const res = assert.commandWorked(db.runCommand(
        {collMod: coll.getName(), index: {keyPattern: kIndexKey, expireAfterSeconds: 1111}}));

    assertIndexExists(coll, kIndexKey, {expireAfterSeconds: 1111}, [mongos, shard1]);
    assertIndexDoesntExist(coll, kIndexKey, {expireAfterSeconds: 1111}, [shard0_ps, shard2]);
}

st.stop();
