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

// Simple shard key on the metadata field.
(function metaShardKey() {
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

    st.shardColl('system.buckets.ts',
                 {meta: 1} /* Shard key */,
                 {meta: 10} /* Split at */,
                 {meta: 10} /* Move the chunk containing {meta: 10} to its own shard */,
                 dbName, /* dbName */
                 true /* Wait until documents orphaned by the move get deleted */);

    let counts = st.chunkCounts('system.buckets.ts', 'test');
    assert.eq(1, counts[st.shard0.shardName]);
    assert.eq(1, counts[st.shard1.shardName]);

    // Query with shard key
    assert.docEq([docs[0]], sDB.ts.find({hostId: 0}).toArray());
    assert.docEq([docs[numDocs - 1]], sDB.ts.find({hostId: (numDocs - 1)}).toArray());

    // Query without shard key
    assert.docEq(docs, sDB.ts.find().sort({time: 1}).toArray());

    assert.commandWorked(sDB.dropDatabase());
})();

// Create a time-series collection with a non-default collation, but an index with the simple
// collation, which makes it eligible as a shard key.
(function metaShardKeyCollation() {
    assert.commandWorked(sDB.createCollection('ts', {
        timeseries: {timeField: 'time', metaField: 'hostName'},
        collation: {locale: 'en', strength: 1, numericOrdering: true}
    }));

    // Insert directly on the primary shard because mongos does not know how to insert into a TS
    // collection.
    st.ensurePrimaryShard(dbName, st.shard0.shardName);
    const tsColl = st.shard0.getDB(dbName).ts;

    const numDocs = 20;
    let docs = [];
    for (let i = 0; i < numDocs; i++) {
        const doc = {
            time: ISODate(),
            hostName: 'host_' + i,
            _id: i,
            data: Random.rand(),
        };
        docs.push(doc);
        assert.commandWorked(tsColl.insert(doc));
    }

    // This index gets created as {meta: 1} on the buckets collection.
    assert.commandWorked(tsColl.createIndex({hostName: 1}, {collation: {locale: 'simple'}}));

    st.shardColl('system.buckets.ts',
                 {meta: 1} /* Shard key */,
                 {meta: 'host_10'} /* Split at */,
                 {meta: 'host_10'} /* Move the chunk containing {meta: 10} to its own shard */,
                 dbName, /* dbName */
                 true /* Wait until documents orphaned by the move get deleted */);

    let counts = st.chunkCounts('system.buckets.ts', 'test');
    assert.eq(1, counts[st.shard0.shardName]);
    assert.eq(1, counts[st.shard1.shardName]);

    // Query with shard key
    assert.docEq([docs[0]], sDB.ts.find({hostName: 'host_0'}).toArray());
    assert.docEq([docs[numDocs - 1]], sDB.ts.find({hostName: 'host_' + (numDocs - 1)}).toArray());

    // Query without shard key
    assert.docEq(docs, sDB.ts.find().sort({time: 1}).toArray());
    assert.commandWorked(sDB.dropDatabase());
})();

// Create a time-series collection with a shard key compounded with a metadata subfield and time.
(function compoundShardKey() {
    assert.commandWorked(
        sDB.createCollection('ts', {timeseries: {timeField: 'time', metaField: 'meta'}}));

    // Insert directly on the primary shard because mongos does not know how to insert into a TS
    // collection.
    st.ensurePrimaryShard(dbName, st.shard0.shardName);

    const tsColl = st.shard0.getDB(dbName).ts;
    const numDocs = 20;
    let docs = [];
    for (let i = 0; i < numDocs; i++) {
        const doc = {
            time: ISODate(),
            meta: {id: i},
            _id: i,
            data: Random.rand(),
        };
        docs.push(doc);
        assert.commandWorked(tsColl.insert(doc));
    }

    // This index gets created as {meta.id: 1, control.min.time: 1, control.max.time: 1} on the
    // buckets collection.
    assert.commandWorked(tsColl.createIndex({'meta.id': 'hashed', time: 1}));

    assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));
    assert.commandWorked(st.s.adminCommand({
        shardCollection: 'test.system.buckets.ts',
        key: {'meta.id': 'hashed', 'control.min.time': 1, 'control.max.time': 1}
    }));

    let counts = st.chunkCounts('system.buckets.ts', 'test');
    assert.eq(1, counts[st.shard0.shardName], counts);
    assert.eq(0, counts[st.shard1.shardName], counts);

    // Split the chunk based on 'bounds' and verify total chunks increased by one.
    const lowestChunk = findChunksUtil.findChunksByNs(configDB, 'test.system.buckets.ts')
                            .sort({min: 1})
                            .limit(1)
                            .next();
    assert(lowestChunk);

    assert.commandWorked(st.s.adminCommand(
        {split: 'test.system.buckets.ts', bounds: [lowestChunk.min, lowestChunk.max]}));

    let otherShard = st.getOther(st.getPrimaryShard(dbName)).name;
    assert.commandWorked(st.s.adminCommand({
        movechunk: 'test.system.buckets.ts',
        find: {'meta.id': 10, 'control.min.time': 0, 'control.max.time': 0},
        to: otherShard,
        _waitForDelete: true
    }));

    counts = st.chunkCounts('system.buckets.ts', 'test');
    assert.eq(1, counts[st.shard0.shardName], counts);
    assert.eq(1, counts[st.shard1.shardName], counts);

    // Query with shard key
    assert.docEq([docs[0]], sDB.ts.find({'meta.id': 0}).toArray());
    assert.docEq([docs[numDocs - 1]], sDB.ts.find({'meta.id': (numDocs - 1)}).toArray());

    // Query without shard key
    assert.docEq(docs, sDB.ts.find().sort({time: 1}).toArray());
    assert.commandWorked(sDB.dropDatabase());
})();

st.stop();
})();
