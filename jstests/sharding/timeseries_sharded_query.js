/**
 * Tests that time-series collections can be sharded and that queries return correct results.
 *
 * @tags: [
 *   requires_fcv_49,
 *   requires_find_command,
 * ]
 */

(function() {
load("jstests/core/timeseries/libs/timeseries.js");
load("jstests/sharding/libs/find_chunks_util.js");

Random.setRandomSeed();

// We have to disable this check because a test manually crafts a sharded timeseries
// collection, which are frozen in 5.0 so we cannot verify anything from it.
TestData.skipCheckingIndexesConsistentAcrossCluster = true;

const st = new ShardingTest({shards: 2});

const dbName = 'test';
const sDB = st.s.getDB(dbName);

if (!TimeseriesTest.timeseriesCollectionsEnabled(st.shard0)) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    st.stop();
    return;
}

(function timeseriesCollectionsCannotBeSharded() {
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));

    assert.commandWorked(
        sDB.createCollection('ts', {timeseries: {timeField: 'time', metaField: 'hostId'}}));

    // Insert directly on the primary shard because mongos does not know how to insert into a TS
    // collection.
    st.ensurePrimaryShard(dbName, st.shard0.shardName);
    const tsColl = st.shard0.getDB(dbName).ts;
    const numDocs = 20;
    let docs = [];
    for (let i = 0; i < numDocs; i++) {
        const doc = {
            time: ISODate(),
            hostId: i,
            _id: i,
            data: Random.rand(),
        };
        docs.push(doc);
        assert.commandWorked(tsColl.insert(doc));
    }

    // This index gets created as {meta: 1} on the buckets collection.
    assert.commandWorked(tsColl.createIndex({hostId: 1}));

    // Trying to shard a time-series collection -> error
    assert.commandFailed(st.s.adminCommand({shardCollection: 'test.ts', key: {hostId: 1}}));

    // Trying to shard the buckets collection -> error
    assert.commandFailed(
        st.s.adminCommand({shardCollection: 'test.system.buckets.ts', key: {meta: 1}}));

    assert.commandWorked(sDB.dropDatabase());
})();

(function manuallyCraftedShardedTimeseriesCollectionCannotBeUsed() {
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));

    assert.commandWorked(sDB.createCollection('coll'));

    st.ensurePrimaryShard(dbName, st.shard0.shardName);
    for (let i = 0; i < 20; i++) {
        assert.commandWorked(st.shard0.getDB(dbName).coll.insert({a: i}));
    }

    assert.commandWorked(st.shard0.getDB(dbName).coll.createIndex({a: 1}));
    assert.commandWorked(st.s.adminCommand({shardCollection: 'test.coll', key: {a: 1}}));

    // It modifies the time-series metadata on the CS and on the shards
    let modifyTimeseriesMetadata = (opTimeseriesMetadata) => {
        st.s.getDB('config').collections.update({_id: 'test.coll'}, opTimeseriesMetadata);
        st.shard0.getDB('config').cache.collections.update({_id: 'test.coll'},
                                                           opTimeseriesMetadata);
        st.shard1.getDB('config').cache.collections.update({_id: 'test.coll'},
                                                           opTimeseriesMetadata);
    };

    // It forces a bump of the collection version moving the {a: 0} chunk to destShardName
    let bumpCollectionVersionThroughMoveChunk = (destShardName) => {
        assert.commandWorked(
            st.s.adminCommand({moveChunk: 'test.coll', find: {a: 0}, to: destShardName}));
    };

    // It forces a refresh of the routing info on the shards
    let forceRefreshOnShards = () => {
        assert.commandWorked(st.shard0.adminCommand(
            {_flushRoutingTableCacheUpdates: 'test.coll', syncFromConfig: true}));
        assert.commandWorked(st.shard1.adminCommand(
            {_flushRoutingTableCacheUpdates: 'test.coll', syncFromConfig: true}));
    };

    // Hacky code to simulate that 'test.coll' is a sharded time-series collection
    modifyTimeseriesMetadata({$set: {timeseriesFields: {timeField: "a"}}});
    bumpCollectionVersionThroughMoveChunk(st.shard1.shardName);
    forceRefreshOnShards();

    let check = (cmdRes) => {
        assert.commandFailedWithCode(cmdRes, ErrorCodes.NotImplemented);
    };

    // CRUD ops & drop collection
    check(st.s.getDB(dbName).runCommand({find: 'coll', filter: {a: 1}}));
    check(st.s.getDB(dbName).runCommand({find: 'coll', filter: {}}));
    check(st.s.getDB(dbName).runCommand({insert: 'coll', documents: [{a: 21}]}));
    check(st.s.getDB(dbName).runCommand({insert: 'coll', documents: [{a: 21}, {a: 22}]}));
    check(st.s.getDB(dbName).runCommand(
        {update: 'coll', updates: [{q: {a: 1}, u: {$set: {b: 10}}}]}));
    check(st.s.getDB(dbName).runCommand({update: 'coll', updates: [{q: {}, u: {$set: {b: 10}}}]}));
    check(st.s.getDB(dbName).runCommand({delete: 'coll', deletes: [{q: {a: 1}, limit: 1}]}));
    check(st.s.getDB(dbName).runCommand({delete: 'coll', deletes: [{q: {}, limit: 0}]}));
    check(st.s.getDB(dbName).runCommand({drop: 'coll'}), ErrorCodes.IllegalOperation);
})();

st.stop();
})();
