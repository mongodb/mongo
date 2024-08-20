/*
 * Test to validate the $_internalAllCollectionStats stage for storageStats.
 *
 * @tags: [
 *   requires_fcv_60,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

function checkResults(aggregationPipeline, checksToDo) {
    assert.soon(() => {
        const results = adminDb.aggregate(aggregationPipeline).toArray();
        assert.lte(numCollections, results.length);

        for (let i = 0; i < numCollections; i++) {
            try {
                const coll = "coll" + i;

                // To check that the data retrieve from $_internalAllCollectionStats is correct we
                // will call $collStats for each namespace to retrieve its storage stats and compare
                // the two outputs.
                const expectedResults = testDb.getCollection(coll)
                                            .aggregate([{$collStats: {storageStats: {}}}])
                                            .toArray();
                assert.neq(null, expectedResults);
                assert.eq(1, expectedResults.length);

                let exists = false;
                for (const data of results) {
                    const ns = data.ns;
                    if (dbName + "." + coll === ns) {
                        checksToDo(data, expectedResults);
                        exists = true;
                        break;
                    }
                }
                assert(exists,
                       "Expected to have $_internalAllCollectionStats results for coll" + i);
            } catch (e) {
                // As we perform two logical executions of $collStats they might return different
                // storageSizes since WT may have rewritten the file during a checkpoint or
                // background compaction. We retry the operation as it is a transient error.
                jsTest.log(e);
                return false;
            }
        }
        return true;
    });
}

// Configure initial sharding cluster
const st = new ShardingTest({shards: 2});
const mongos = st.s;

const dbName = "test";
const testDb = mongos.getDB(dbName);
const adminDb = mongos.getDB("admin");
const numCollections = 4;

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
    const aggregationPipeline = [{$_internalAllCollectionStats: {stats: {storageStats: {}}}}];

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
    checkResults(aggregationPipeline, checksToDo);
})();

// Tests to check the correct behaviour of a $project stage after $_internalAllCollectionStats
(function testNumOrphanDocsFieldProject() {
    const aggregationPipeline = [
        {$_internalAllCollectionStats: {stats: {storageStats: {}}}},
        {$project: {"ns": 1, "storageStats.numOrphanDocs": 1}}
    ];

    const checksToDo = (left, right) => {
        assert.eq(left.storageStats.numOrphanDocs,
                  right[0].storageStats.numOrphanDocs,
                  "Expected same output after a projection with storageStats.numOrphanDocs field");
    };
    checkResults(aggregationPipeline, checksToDo);
})();

(function testStorageSizeFieldProject() {
    const aggregationPipeline = [
        {$_internalAllCollectionStats: {stats: {storageStats: {}}}},
        {$project: {"ns": 1, "storageStats.storageSize": 1}}
    ];

    const checksToDo = (left, right) => {
        assert.eq(left.storageStats.storageSize,
                  right[0].storageStats.storageSize,
                  "Expected same output after a projection with storageStats.storageSize field");
    };
    checkResults(aggregationPipeline, checksToDo);
})();

(function testNIndexesFieldProject() {
    const aggregationPipeline = [
        {$_internalAllCollectionStats: {stats: {storageStats: {}}}},
        {$project: {"ns": 1, "storageStats.nindexes": 1}}
    ];

    const checksToDo = (left, right) => {
        assert.eq(left.storageStats.nindexes,
                  right[0].storageStats.nindexes,
                  "Expected same output after a projection with storageStats.nindexes field");
    };
    checkResults(aggregationPipeline, checksToDo);
})();

(function testTotalSizeFieldProject() {
    const aggregationPipeline = [
        {$_internalAllCollectionStats: {stats: {storageStats: {}}}},
        {$project: {"ns": 1, "storageStats.totalSize": 1}}
    ];

    const checksToDo = (left, right) => {
        assert.eq(left.storageStats.totalSize,
                  right[0].storageStats.totalSize,
                  "Expected same output after a projection with storageStats.totalSize field");
    };
    checkResults(aggregationPipeline, checksToDo);
})();

(function testProjectingDifferentFields() {
    const aggregationPipeline = [
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
    ];

    const checksToDo = (left, right) => {
        const msg = "Expected same output after a projection with fields from different storage " +
            "stats groups";
        assert.eq(left.storageStats.numOrphanDocs, right[0].storageStats.numOrphanDocs, msg);
        assert.eq(left.storageStats.storageSize, right[0].storageStats.storageSize, msg);
        assert.eq(left.storageStats.nindexes, right[0].storageStats.nindexes, msg);
        assert.eq(left.storageStats.totalSize, right[0].storageStats.totalSize, msg);
    };
    checkResults(aggregationPipeline, checksToDo);
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
