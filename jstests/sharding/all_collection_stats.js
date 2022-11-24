/*
 * Test to validate the $_internalAllCollectionStats stage for storageStats.
 *
 * @tags: [
 *   requires_fcv_62,
 * ]
 */

(function() {
'use strict';

function checkResults(results, checksToDo) {
    for (let i = 0; i < numCollections; i++) {
        const coll = "coll" + i;

        // To check that the data retrieve from $_internalAllCollectionStats is correct we will call
        // $collStats for each namespace to retrieve its storage stats and compare the two outputs.
        const expectedResults =
            testDb.getCollection(coll).aggregate([{$collStats: {storageStats: {}}}]).toArray();
        assert.neq(null, expectedResults);
        assert.eq(expectedResults.length, 1);

        let exists = false;
        for (const data of results) {
            const ns = data.ns;
            if (dbName + "." + coll === ns) {
                checksToDo(data, expectedResults);
                exists = true;
                break;
            }
        }
        assert(exists, "Expected to have $_internalAllCollectionStats results for coll" + i);
    }
}

// Configure initial sharding cluster
const st = new ShardingTest({shards: 2});
const mongos = st.s;

const dbName = "test";
const testDb = mongos.getDB(dbName);
const adminDb = mongos.getDB("admin");
const numCollections = 20;

// Insert sharded collections to validate the aggregation stage
for (let i = 0; i < (numCollections / 2); i++) {
    const coll = "coll" + i;
    assert(st.adminCommand({shardcollection: dbName + "." + coll, key: {skey: 1}}));
    assert.commandWorked(testDb.getCollection(coll).insert({skey: i}));
}

// Insert some unsharded collections to validate the aggregation stage
for (let i = numCollections / 2; i < numCollections; i++) {
    const coll = "coll" + i;
    assert.commandWorked(testDb.getCollection(coll).insert({skey: i}));
}

// Testing for comparing each collection returned from $_internalAllCollectionStats to $collStats
(function testInternalAllCollectionStats() {
    const outputData =
        adminDb.aggregate([{$_internalAllCollectionStats: {stats: {storageStats: {}}}}]).toArray();
    assert.gte(outputData.length, 20);

    const checksToDo = (left, right) => {
        const msg = "Expected same output from $_internalAllCollectionStats and $collStats " +
            "for same namespace";
        assert.eq(left.host, right[0].host, msg);
        assert.eq(left.shard, right[0].shard, msg);
        assert.eq(left.storageStats.size, right[0].storageStats.size, msg);
        assert.eq(left.storageStats.count, right[0].storageStats.count, msg);
        assert.eq(left.storageStats.avgObjSize, right[0].storageStats.avgObjSize, msg);
        assert.eq(left.storageStats.storageSize, right[0].storageStats.storageSize, msg);
        assert.eq(left.storageStats.freeStorageSize, right[0].storageStats.freeStorageSize, msg);
        assert.eq(left.storageStats.nindexes, right[0].storageStats.nindexes, msg);
        assert.eq(left.storageStats.totalIndexSize, right[0].storageStats.totalIndexSize, msg);
        assert.eq(left.storageStats.totalSize, right[0].storageStats.totalSize, msg);
    };
    checkResults(outputData, checksToDo);
})();

// Tests to check the correct behaviour of a $project stage after $_internalAllCollectionStats
(function testNumOrphanDocsFieldProject() {
    const outputData = adminDb
                           .aggregate([
                               {$_internalAllCollectionStats: {stats: {storageStats: {}}}},
                               {$project: {"ns": 1, "storageStats.numOrphanDocs": 1}}
                           ])
                           .toArray();
    assert.gte(outputData.length, numCollections);

    const checksToDo = (left, right) => {
        assert.eq(left.storageStats.numOrphanDocs,
                  right[0].storageStats.numOrphanDocs,
                  "Expected same output after a projection with storageStats.numOrphanDocs field");
    };
    checkResults(outputData, checksToDo);
})();

(function testStorageSizeFieldProject() {
    const outputData = adminDb
                           .aggregate([
                               {$_internalAllCollectionStats: {stats: {storageStats: {}}}},
                               {$project: {"ns": 1, "storageStats.storageSize": 1}}
                           ])
                           .toArray();
    assert.gte(outputData.length, numCollections);

    const checksToDo = (left, right) => {
        assert.eq(left.storageStats.storageSize,
                  right[0].storageStats.storageSize,
                  "Expected same output after a projection with storageStats.storageSize field");
    };
    checkResults(outputData, checksToDo);
})();

(function testNIndexesFieldProject() {
    const outputData = adminDb
                           .aggregate([
                               {$_internalAllCollectionStats: {stats: {storageStats: {}}}},
                               {$project: {"ns": 1, "storageStats.nindexes": 1}}
                           ])
                           .toArray();
    assert.gte(outputData.length, numCollections);

    const checksToDo = (left, right) => {
        assert.eq(left.storageStats.nindexes,
                  right[0].storageStats.nindexes,
                  "Expected same output after a projection with storageStats.nindexes field");
    };
    checkResults(outputData, checksToDo);
})();

(function testTotalSizeFieldProject() {
    const outputData = adminDb
                           .aggregate([
                               {$_internalAllCollectionStats: {stats: {storageStats: {}}}},
                               {$project: {"ns": 1, "storageStats.totalSize": 1}}
                           ])
                           .toArray();
    assert.gte(outputData.length, numCollections);

    const checksToDo = (left, right) => {
        assert.eq(left.storageStats.totalSize,
                  right[0].storageStats.totalSize,
                  "Expected same output after a projection with storageStats.totalSize field");
    };
    checkResults(outputData, checksToDo);
})();

(function testProjectingDifferentFields() {
    const outputData = adminDb
                           .aggregate([
                               {$_internalAllCollectionStats: {stats: {storageStats: {}}}},
                               {
                                   $project: {
                                       "ns": 1,
                                       "storageStats.numOrphanDocs": 1,
                                       "storageStats.storageSize": 1,
                                       "storageStats.nindexes": 1,
                                       "storageStats.totalSize": 1
                                   }
                               }
                           ])
                           .toArray();
    assert.gte(outputData.length, numCollections);

    const checksToDo = (left, right) => {
        const msg = "Expected same output after a projection with fields from different storage " +
            "stats groups";
        assert.eq(left.storageStats.numOrphanDocs, right[0].storageStats.numOrphanDocs, msg);
        assert.eq(left.storageStats.storageSize, right[0].storageStats.storageSize, msg);
        assert.eq(left.storageStats.nindexes, right[0].storageStats.nindexes, msg);
        assert.eq(left.storageStats.totalSize, right[0].storageStats.totalSize, msg);
    };
    checkResults(outputData, checksToDo);
})();

// Test valid query with empty specification
assert.commandWorked(
    adminDb.runCommand({aggregate: 1, pipeline: [{$_internalAllCollectionStats: {}}], cursor: {}}));

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
