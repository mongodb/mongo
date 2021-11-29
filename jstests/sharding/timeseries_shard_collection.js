/**
 * Tests that time-series collections can be sharded with different configurations.
 *
 * @tags: [
 *   requires_fcv_50,
 *   requires_find_command,
 * ]
 */

(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

Random.setRandomSeed();

const st = new ShardingTest({shards: 2, rs: {nodes: 2}});

const dbName = 'test';
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
const sDB = st.s.getDB(dbName);
const timeseries = {
    timeField: 'time',
    metaField: 'hostId',
};

if (!TimeseriesTest.timeseriesCollectionsEnabled(st.shard0)) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    st.stop();
    return;
}

if (TimeseriesTest.shardedtimeseriesCollectionsEnabled(st.shard0)) {
    function validateBucketsCollectionSharded({collName, shardKey}) {
        const configColls = st.s.getDB('config').collections;
        const output = configColls
                           .find({
                               _id: 'test.system.buckets.' + collName,
                               key: shardKey,
                               timeseriesFields: {$exists: true},
                           })
                           .toArray();
        assert.eq(output.length, 1, configColls.find().toArray());
        assert.eq(output[0].timeseriesFields.timeField, timeseries.timeField, output[0]);
        assert.eq(output[0].timeseriesFields.metaField, timeseries.metaField, output[0]);
    }

    function validateViewCreated(viewName) {
        const views =
            sDB.runCommand({listCollections: 1, filter: {type: 'timeseries', name: viewName}})
                .cursor.firstBatch;
        assert.eq(views.length, 1, views);

        const tsOpts = views[0].options.timeseries;
        assert.eq(tsOpts.timeField, timeseries.timeField, tsOpts);
        assert.eq(tsOpts.metaField, timeseries.metaField, tsOpts);
    }

    // Simple shard key on the metadata field.
    function metaShardKey(implicit) {
        // Command should fail since the 'timeseries' specification does not match that existing
        // collection.
        if (!implicit) {
            assert.commandWorked(sDB.createCollection('ts', {timeseries}));
            // This index gets created as {meta: 1} on the buckets collection.
            assert.commandWorked(sDB.ts.createIndex({hostId: 1}));
            assert.commandFailedWithCode(st.s.adminCommand({
                shardCollection: 'test.ts',
                key: {'hostId': 1},
                timeseries: {timeField: 'time'},
            }),
                                         5731500);
        }

        assert.commandWorked(
            st.s.adminCommand({shardCollection: 'test.ts', key: {'hostId': 1}, timeseries}));

        validateBucketsCollectionSharded({collName: 'ts', shardKey: {meta: 1}, timeseries});

        validateViewCreated("ts");

        assert.commandWorked(
            st.s.adminCommand({split: 'test.system.buckets.ts', middle: {meta: 10}}));

        const primaryShard = st.getPrimaryShard(dbName);
        assert.commandWorked(st.s.adminCommand({
            movechunk: 'test.system.buckets.ts',
            find: {meta: 10},
            to: st.getOther(primaryShard).shardName,
            _waitForDelete: true,
        }));

        let counts = st.chunkCounts('system.buckets.ts', 'test');
        assert.eq(1, counts[st.shard0.shardName]);
        assert.eq(1, counts[st.shard1.shardName]);

        assert(sDB.ts.drop());
    }

    // Sharding an existing timeseries collection.
    metaShardKey(false);

    // Sharding a new timeseries collection.
    metaShardKey(true);

    // Shard key on the metadata field and time fields.
    function metaAndTimeShardKey(implicit) {
        assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));

        if (!implicit) {
            assert.commandWorked(sDB.createCollection('ts', {timeseries}));
        }

        assert.commandWorked(st.s.adminCommand({
            shardCollection: 'test.ts',
            key: {'hostId': 1, 'time': 1},
            timeseries,
        }));

        validateViewCreated("ts");

        validateBucketsCollectionSharded({
            collName: 'ts',
            // The 'time' field should be translated to 'control.min.time' on buckets collection.
            shardKey: {meta: 1, 'control.min.time': 1},
            timeseries,
        });

        assert.commandWorked(st.s.adminCommand(
            {split: 'test.system.buckets.ts', middle: {meta: 10, 'control.min.time': MinKey}}));

        const primaryShard = st.getPrimaryShard(dbName);
        assert.commandWorked(st.s.adminCommand({
            movechunk: 'test.system.buckets.ts',
            find: {meta: 10, 'control.min.time': MinKey},
            to: st.getOther(primaryShard).shardName,
            _waitForDelete: true,
        }));

        let counts = st.chunkCounts('system.buckets.ts', 'test');
        assert.eq(1, counts[st.shard0.shardName]);
        assert.eq(1, counts[st.shard1.shardName]);

        assert(sDB.ts.drop());
    }

    // Sharding an existing timeseries collection.
    metaAndTimeShardKey(false);

    // Sharding a new timeseries collection.
    metaAndTimeShardKey(true);

    function timeseriesInsert(coll) {
        let insertCount = 0;
        for (let i = 10; i < 100; i++) {
            assert.commandWorked(coll.insert([
                {hostId: 10, time: ISODate(`19` + i + `-01-01`)},
                {hostId: 11, time: ISODate(`19` + i + `-01-01`)},
                {hostId: 12, time: ISODate(`19` + i + `-01-01`)},
                {hostId: 13, time: ISODate(`19` + i + `-01-01`)},
                {hostId: 14, time: ISODate(`19` + i + `-01-01`)},
                {hostId: 15, time: ISODate(`19` + i + `-01-01`)},
                {hostId: 16, time: ISODate(`19` + i + `-01-01`)},
                {hostId: 17, time: ISODate(`19` + i + `-01-01`)},
                {hostId: 18, time: ISODate(`19` + i + `-01-01`)},
                {hostId: 19, time: ISODate(`19` + i + `-01-01`)}
            ]));
            insertCount += 10;
        }
        return insertCount;
    }

    // Shard key on the hashed field.

    function runShardKeyPatternValidation(collectionExists) {
        (function hashAndTimeShardKey() {
            if (collectionExists) {
                assert.commandWorked(sDB.createCollection(
                    'ts', {timeseries: {timeField: 'time', metaField: 'hostId'}}));
            }

            // Only range is allowed on time field.
            assert.commandFailedWithCode(st.s.adminCommand({
                shardCollection: 'test.ts',
                key: {time: 'hashed'},
                timeseries: {timeField: 'time', metaField: 'hostId'},
            }),
                                         ErrorCodes.BadValue);

            if (!collectionExists) {
                assert.commandWorked(sDB.createCollection(
                    'ts', {timeseries: {timeField: 'time', metaField: 'hostId'}}));
            }
            let coll = sDB.getCollection('ts');
            assert.commandWorked(coll.insert([
                {hostId: 10, time: ISODate(`1901-01-01`)},
                {hostId: 11, time: ISODate(`1902-01-01`)},
            ]));
            assert.commandWorked(coll.createIndex({hostId: 'hashed'}));

            assert.commandWorked(st.s.adminCommand({
                shardCollection: 'test.ts',
                key: {hostId: 'hashed'},
                timeseries: {timeField: 'time', metaField: 'hostId'}
            }));

            validateBucketsCollectionSharded({
                collName: 'ts',
                shardKey: {meta: 'hashed'},
                timeSeriesParams: {timeField: 'time', metaField: 'hostId'}
            });

            assert.eq(coll.find().itcount(), 2);  // Validate count after sharding.
            let insertCount = timeseriesInsert(coll);
            assert.eq(coll.find().itcount(), insertCount + 2);
            coll.drop();

            if (collectionExists) {
                assert.commandWorked(sDB.createCollection(
                    'ts', {timeseries: {timeField: 'time', metaField: 'hostId'}}));
            }
            assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));

            // Sharding key with hashed meta field and time field.
            assert.commandWorked(st.s.adminCommand({
                shardCollection: 'test.ts',
                key: {hostId: 'hashed', time: 1},
                timeseries: {timeField: 'time', metaField: 'hostId'},
                numInitialChunks: 2
            }));

            coll = sDB.getCollection('ts');
            assert.eq(coll.find().itcount(), 0);
            insertCount = timeseriesInsert(coll);
            assert.eq(coll.find().itcount(), insertCount);
            coll.drop();
        })();

        // Test that invalid shard keys fail.
        (function invalidShardKeyPatterns() {
            if (collectionExists) {
                assert.commandWorked(sDB.createCollection(
                    'ts', {timeseries: {timeField: 'time', metaField: 'hostId'}}));
            }

            // No other fields, including _id, are allowed in the shard key pattern
            assert.commandFailedWithCode(st.s.adminCommand({
                shardCollection: 'test.ts',
                key: {_id: 1},
                timeseries: {timeField: 'time', metaField: 'hostId'},
            }),
                                         5914001);

            assert.commandFailedWithCode(st.s.adminCommand({
                shardCollection: 'test.ts',
                key: {_id: 1, time: 1},
                timeseries: {timeField: 'time', metaField: 'hostId'},
            }),
                                         5914001);

            assert.commandFailedWithCode(st.s.adminCommand({
                shardCollection: 'test.ts',
                key: {_id: 1, hostId: 1},
                timeseries: {timeField: 'time', metaField: 'hostId'},
            }),
                                         5914001);

            assert.commandFailedWithCode(st.s.adminCommand({
                shardCollection: 'test.ts',
                key: {a: 1},
                timeseries: {timeField: 'time', metaField: 'hostId'},
            }),
                                         5914001);

            // Shared key where time is not the last field in shard key should fail.
            assert.commandFailedWithCode(st.s.adminCommand({
                shardCollection: 'test.ts',
                key: {time: 1, hostId: 1},
                timeseries: {timeField: 'time', metaField: 'hostId'}
            }),
                                         5914000);
            assert(sDB.getCollection("ts").drop());
        })();

        (function noMetaFieldTimeseries() {
            if (collectionExists) {
                assert.commandWorked(sDB.createCollection('ts', {timeseries: {timeField: 'time'}}));
            }

            assert.commandFailedWithCode(st.s.adminCommand({
                shardCollection: 'test.ts',
                key: {_id: 1},
                timeseries: {timeField: 'time'},
            }),
                                         5914001);

            assert.commandFailedWithCode(st.s.adminCommand({
                shardCollection: 'test.ts',
                key: {a: 1},
                timeseries: {timeField: 'time'},
            }),
                                         5914001);

            assert.commandWorked(st.s.adminCommand(
                {shardCollection: 'test.ts', key: {time: 1}, timeseries: {timeField: 'time'}}));

            assert(sDB.getCollection("ts").drop());
        })();
    }

    runShardKeyPatternValidation(true);
    runShardKeyPatternValidation(false);

    // Verify that the shardCollection command fails if the 'system.buckets' collection does not
    // have time-series options.
    sDB.getCollection("ts").drop();
    sDB.createCollection("system.buckets.ts");
    assert.commandFailedWithCode(st.s.adminCommand({
        shardCollection: 'test.ts',
        key: {time: 1},
    }),
                                 6159000);
    assert.commandFailedWithCode(
        st.s.adminCommand(
            {shardCollection: 'test.ts', key: {time: 1}, timeseries: {timeField: 'time'}}),
        6159000);

    // Cannot shard a system namespace.
    assert.commandFailedWithCode(st.s.adminCommand({
        shardCollection: 'test.system.bucket.ts',
        key: {time: 1},
    }),
                                 ErrorCodes.IllegalOperation);

} else {
    (function timeseriesCollectionsCannotBeSharded() {
        assert.commandFailedWithCode(
            st.s.adminCommand({shardCollection: 'test.ts', key: {meta: 1}, timeseries}), 5731502);

        assert.commandWorked(sDB.createCollection('ts', {timeseries}));

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

        assert(tsColl.drop());
    })();
}

st.stop();
})();
