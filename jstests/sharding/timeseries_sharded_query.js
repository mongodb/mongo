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

const st = new ShardingTest({shards: 2, rs: {nodes: 2}});

const dbName = 'test';
const sDB = st.s.getDB(dbName);
const configDB = st.s0.getDB('config');

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

st.stop();
})();
