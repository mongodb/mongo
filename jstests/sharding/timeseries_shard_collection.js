/**
 * Tests that time-series collections can be sharded with different configurations.
 *
 * @tags: [
 *   requires_fcv_49,
 *   disabled_due_to_server_58295
 * ]
 */

(function() {
load("jstests/core/timeseries/libs/timeseries.js");

Random.setRandomSeed();

const st = new ShardingTest({shards: 2, rs: {nodes: 2}});

const dbName = 'test';
const sDB = st.s.getDB(dbName);

if (!TimeseriesTest.timeseriesCollectionsEnabled(st.shard0)) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    st.stop();
    return;
}

if (TimeseriesTest.shardedtimeseriesCollectionsEnabled(st.shard0)) {
    function validateBucketsCollectionSharded({collName, shardKey, timeSeriesParams}) {
        const configColls = st.s.getDB('config').collections;
        const output = configColls
                           .find({
                               _id: 'test.system.buckets.' + collName,
                               key: shardKey,
                               timeseriesFields: {$exists: true}
                           })
                           .toArray();
        assert.eq(output.length, 1, configColls.find().toArray());
        assert.eq(output[0].timeseriesFields.timeField, timeSeriesParams.timeField, output[0]);
        assert.eq(output[0].timeseriesFields.metaField, timeSeriesParams.metaField, output[0]);
    }

    // Simple shard key on the metadata field.
    (function metaShardKey() {
        assert.commandWorked(
            sDB.createCollection('ts', {timeseries: {timeField: 'time', metaField: 'hostId'}}));

        assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));

        // This index gets created as {meta: 1} on the buckets collection.
        assert.commandWorked(sDB.ts.createIndex({hostId: 1}));

        // Command should fail since the 'timeseries' specification does not match that existing
        // collection.
        assert.commandFailedWithCode(
            st.s.adminCommand(
                {shardCollection: 'test.ts', key: {'hostId': 1}, timeseries: {timeField: 'time'}}),
            5731500);

        assert.commandWorked(st.s.adminCommand({
            shardCollection: 'test.ts',
            key: {'hostId': 1},
            timeseries: {timeField: 'time', metaField: 'hostId'}
        }));

        validateBucketsCollectionSharded({
            collName: 'ts',
            shardKey: {meta: 1},
            timeSeriesParams: {timeField: 'time', metaField: 'hostId'}
        });

        assert.commandWorked(
            st.s.adminCommand({split: 'test.system.buckets.ts', middle: {meta: 10}}));

        const primaryShard = st.getPrimaryShard(dbName);
        assert.commandWorked(st.s.adminCommand({
            movechunk: 'test.system.buckets.ts',
            find: {meta: 10},
            to: st.getOther(primaryShard).shardName,
            _waitForDelete: true
        }));

        let counts = st.chunkCounts('system.buckets.ts', 'test');
        assert.eq(1, counts[st.shard0.shardName]);
        assert.eq(1, counts[st.shard1.shardName]);

        sDB.dropDatabase();
    })();

    // Shard key on the metadata field and time fields.
    function metaAndTimeShardKey() {
        assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));
        assert.commandWorked(st.s.adminCommand({
            shardCollection: 'test.ts',
            key: {'hostId': 1, 'time': 1},
            timeseries: {timeField: 'time', metaField: 'hostId'},
        }));

        validateBucketsCollectionSharded({
            collName: 'ts',
            // The 'time' field should be translated to 'control.min.time' on buckets collection.
            shardKey: {meta: 1, 'control.min.time': 1},
            timeSeriesParams: {timeField: 'time', metaField: 'hostId'}
        });

        assert.commandWorked(st.s.adminCommand(
            {split: 'test.system.buckets.ts', middle: {meta: 10, 'control.min.time': MinKey}}));

        const primaryShard = st.getPrimaryShard(dbName);
        assert.commandWorked(st.s.adminCommand({
            movechunk: 'test.system.buckets.ts',
            find: {meta: 10, 'control.min.time': MinKey},
            to: st.getOther(primaryShard).shardName,
            _waitForDelete: true
        }));

        let counts = st.chunkCounts('system.buckets.ts', 'test');
        assert.eq(1, counts[st.shard0.shardName]);
        assert.eq(1, counts[st.shard1.shardName]);

        sDB.dropDatabase();
    }

    // Sharding a collection with an existing timeseries collection.
    assert.commandWorked(
        sDB.createCollection('ts', {timeseries: {timeField: 'time', metaField: 'hostId'}}));
    metaAndTimeShardKey();

    // Sharding a collection with a new timeseries collection.
    metaAndTimeShardKey();

} else {
    (function timeseriesCollectionsCannotBeSharded() {
        assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));

        assert.commandFailedWithCode(st.s.adminCommand({
            shardCollection: 'test.ts',
            key: {meta: 1},
            timeseries: {timeField: 'time', metaField: 'hostId'}
        }),
                                     5731502);

        assert.commandWorked(
            sDB.createCollection('ts', {timeseries: {timeField: 'time', metaField: 'hostId'}}));

        assert.commandFailedWithCode(
            st.s.adminCommand({shardCollection: 'test.ts', key: {meta: 1}}), 5731502);

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

        // Trying to shard the buckets collection -> error
        assert.commandFailedWithCode(
            st.s.adminCommand({shardCollection: 'test.system.buckets.ts', key: {meta: 1}}),
            5731501);

        assert.commandWorked(sDB.dropDatabase());
    })();
}

st.stop();
})();
