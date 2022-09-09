/*
 * Test to validate the $_internalAllCollectionStats stage for storageStats.
 *
 * @tags: [
 *   requires_fcv_62,
 * ]
 */

(function() {
'use strict';

// Configure initial sharding cluster
const st = new ShardingTest({shards: 2});
const mongos = st.s;

const dbName = "test";
const testDb = mongos.getDB(dbName);
const adminDb = mongos.getDB("admin");

// Insert sharded collections to validate the aggregation stage
for (let i = 0; i < 10; i++) {
    const coll = "coll" + i;
    assert(st.adminCommand({shardcollection: dbName + "." + coll, key: {skey: 1}}));
    assert.commandWorked(testDb.getCollection(coll).insert({skey: i}));
}

// Insert some unsharded collections to validate the aggregation stage
for (let i = 10; i < 20; i++) {
    const coll = "coll" + i;
    assert.commandWorked(testDb.getCollection(coll).insert({skey: i}));
}

// Get output data
const outputData =
    adminDb.aggregate([{$_internalAllCollectionStats: {stats: {storageStats: {}}}}]).toArray();
assert.gte(outputData.length, 20);

// Testing for comparing each collection returned from $_internalAllCollectionStats to $collStats
for (let i = 0; i < 20; i++) {
    const coll = "coll" + i;
    const expectedResults =
        testDb.getCollection(coll).aggregate([{$collStats: {storageStats: {}}}]).toArray();
    assert.neq(null, expectedResults);
    assert.eq(expectedResults.length, 1);

    let exists = false;
    for (const data of outputData) {
        const ns = data.ns;
        if (dbName + "." + coll === ns) {
            assert.eq(data.host, expectedResults[0].host);
            assert.eq(data.shard, expectedResults[0].shard);
            assert.eq(tojson(data.storageStats), tojson(expectedResults[0].storageStats));
            exists = true;
            break;
        }
    }

    assert(exists);
}

// Test invalid queries/values.
assert.commandFailedWithCode(
    adminDb.runCommand({aggregate: 1, pipeline: [{$_internalAllCollectionStats: 3}], cursor: {}}),
    6789103);

const response = assert.commandFailedWithCode(testDb.runCommand({
    aggregate: "foo",
    pipeline: [{$_internalAllCollectionStats: {stats: {storageStats: {}}}}],
    cursor: {}
}),
                                              6789104);
assert.neq(-1, response.errmsg.indexOf("$_internalAllCollectionStats"), response.errmsg);
assert.neq(-1, response.errmsg.indexOf("admin database"), response.errmsg);

st.stop();
})();
