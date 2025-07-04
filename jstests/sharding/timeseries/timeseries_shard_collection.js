/**
 * Tests that time-series collections can be sharded with different configurations.
 *
 * @tags: [
 *   requires_fcv_51,
 * ]
 */
import {
    getTimeseriesBucketsColl,
    getTimeseriesCollForDDLOps
} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

Random.setRandomSeed();

const st = new ShardingTest({shards: 2, rs: {nodes: 2}});

const dbName = 'test';
const collName = 'ts';
const collNss = dbName + '.' + collName;
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
const sDB = st.s.getDB(dbName);
const timeseries = {
    timeField: 'time',
    metaField: 'hostId',
};

function validateCollectionSharded({shardKey}) {
    const configColls = st.s.getDB('config').collections;
    const output = configColls
                       .find({
                           _id: `${dbName}.${getTimeseriesCollForDDLOps(sDB, collName)}`,
                           key: shardKey,
                           timeseriesFields: {$exists: true},
                       })
                       .toArray();
    assert.eq(output.length, 1, configColls.find().toArray());
    assert.eq(output[0].timeseriesFields.timeField, timeseries.timeField, output[0]);
    assert.eq(output[0].timeseriesFields.metaField, timeseries.metaField, output[0]);
}

function validateTimeseriesCollectionCreated(viewName) {
    const colls = sDB.runCommand({listCollections: 1, filter: {type: 'timeseries', name: viewName}})
                      .cursor.firstBatch;
    assert.eq(colls.length, 1, colls);

    const tsOpts = colls[0].options.timeseries;
    assert.eq(tsOpts.timeField, timeseries.timeField, tsOpts);
    assert.eq(tsOpts.metaField, timeseries.metaField, tsOpts);
}

function validateRawIndexBackingShardKey({expectedKey}) {
    const coll = sDB.getCollection(getTimeseriesCollForRawOps(sDB, collName));
    const indexKeys = coll.getIndexKeys(getRawOperationSpec(sDB));
    const index = indexKeys.filter(i => bsonWoCompare(i, expectedKey) === 0);
    assert.eq(1,
              index.length,
              "Expected one index with the key: " + tojson(expectedKey) +
                  " but found the following indexes: " + tojson(indexKeys));
}

// Simple shard key on the metadata field.
function metaShardKey(implicit) {
    // Command should fail since the 'timeseries' specification does not match that existing
    // collection.
    if (!implicit) {
        assert.commandWorked(sDB.createCollection(collName, {timeseries}));
        assert.commandWorked(sDB.ts.createIndex({hostId: 1}));
        assert.commandFailedWithCode(st.s.adminCommand({
            shardCollection: collNss,
            key: {'hostId': 1},
            timeseries: {timeField: 'time'},
        }),
                                     [ErrorCodes.InvalidOptions]);
    }

    assert.commandWorked(
        st.s.adminCommand({shardCollection: collNss, key: {'hostId': 1}, timeseries}));

    validateCollectionSharded({shardKey: {meta: 1}, timeseries});

    validateTimeseriesCollectionCreated(collName);

    // shardCollection on a new time-series collection will use the default time-series index as the
    // index backing the shardKey.
    const expectedKey =
        implicit ? {"meta": 1, "control.min.time": 1, "control.max.time": 1} : {'meta': 1};
    validateRawIndexBackingShardKey({expectedKey: expectedKey});

    assert.commandWorked(st.s.adminCommand(
        {split: `${dbName}.${getTimeseriesCollForDDLOps(sDB, collName)}`, middle: {meta: 10}}));

    const primaryShard = st.getPrimaryShard(dbName);
    assert.commandWorked(st.s.adminCommand({
        movechunk: `${dbName}.${getTimeseriesCollForDDLOps(sDB, collName)}`,
        find: {meta: 10},
        to: st.getOther(primaryShard).shardName,
        _waitForDelete: true,
    }));

    let counts = st.chunkCounts(collName, dbName);
    assert.eq(1, counts[st.shard0.shardName]);
    assert.eq(1, counts[st.shard1.shardName]);

    assert(sDB.ts.drop());
}

// Sharding an existing timeseries collection.
metaShardKey(false);

// Sharding a new timeseries collection.
metaShardKey(true);

// Simple shard key on a subfield of the metadata field.
function metaSubFieldShardKey(implicit) {
    // Command should fail since the 'timeseries' specification does not match that existing
    // collection.
    if (!implicit) {
        assert.commandWorked(sDB.createCollection(collName, {timeseries}));
        assert.commandWorked(sDB.ts.createIndex({'hostId.a': 1}));
        assert.commandFailedWithCode(st.s.adminCommand({
            shardCollection: collNss,
            key: {'hostId.a': 1},
            timeseries: {timeField: 'time'},
        }),
                                     [ErrorCodes.InvalidOptions]);
    }

    assert.commandWorked(
        st.s.adminCommand({shardCollection: collNss, key: {'hostId.a': 1}, timeseries}));

    validateCollectionSharded({shardKey: {'meta.a': 1}, timeseries});

    validateTimeseriesCollectionCreated(collName);

    validateRawIndexBackingShardKey({expectedKey: {'meta.a': 1}});

    assert.commandWorked(st.s.adminCommand(
        {split: `${dbName}.${getTimeseriesCollForDDLOps(sDB, collName)}`, middle: {'meta.a': 10}}));

    const primaryShard = st.getPrimaryShard(dbName);
    assert.commandWorked(st.s.adminCommand({
        movechunk: `${dbName}.${getTimeseriesCollForDDLOps(sDB, collName)}`,
        find: {'meta.a': 10},
        to: st.getOther(primaryShard).shardName,
        _waitForDelete: true,
    }));

    let counts = st.chunkCounts(collName, dbName);
    assert.eq(1, counts[st.shard0.shardName]);
    assert.eq(1, counts[st.shard1.shardName]);

    assert(sDB.ts.drop());
}

// Sharding an existing timeseries collection.
metaSubFieldShardKey(false);

// Sharding a new timeseries collection.
metaSubFieldShardKey(true);

// Shard key on the metadata field and time fields.
function metaAndTimeShardKey(implicit) {
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));

    if (!implicit) {
        assert.commandWorked(sDB.createCollection(collName, {timeseries}));
    }

    assert.commandWorked(st.s.adminCommand({
        shardCollection: collNss,
        key: {'hostId': 1, 'time': 1},
        timeseries,
    }));

    validateTimeseriesCollectionCreated(collName);

    validateRawIndexBackingShardKey({
        expectedKey: {"meta": 1, "control.min.time": 1, "control.max.time": 1},
    });

    validateCollectionSharded({
        // The 'time' field should be translated to 'control.min.time' on the raw buckets.
        shardKey: {meta: 1, 'control.min.time': 1},
        timeseries,
    });

    assert.commandWorked(st.s.adminCommand({
        split: `${dbName}.${getTimeseriesCollForDDLOps(sDB, collName)}`,
        middle: {meta: 10, 'control.min.time': MinKey}
    }));

    const primaryShard = st.getPrimaryShard(dbName);
    assert.commandWorked(st.s.adminCommand({
        movechunk: `${dbName}.${getTimeseriesCollForDDLOps(sDB, collName)}`,
        find: {meta: 10, 'control.min.time': MinKey},
        to: st.getOther(primaryShard).shardName,
        _waitForDelete: true,
    }));

    let counts = st.chunkCounts(collName, dbName);
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
                collName, {timeseries: {timeField: 'time', metaField: 'hostId'}}));
        }

        // Only range is allowed on time field.
        assert.commandFailedWithCode(st.s.adminCommand({
            shardCollection: collNss,
            key: {time: 'hashed'},
            timeseries: {timeField: 'time', metaField: 'hostId'},
        }),
                                     [880031, ErrorCodes.BadValue]);

        if (!collectionExists) {
            assert.commandWorked(sDB.createCollection(
                collName, {timeseries: {timeField: 'time', metaField: 'hostId'}}));
        }
        let coll = sDB.getCollection(collName);
        assert.commandWorked(coll.insert([
            {hostId: 10, time: ISODate(`1901-01-01`)},
            {hostId: 11, time: ISODate(`1902-01-01`)},
        ]));
        assert.commandWorked(coll.createIndex({hostId: 'hashed'}));

        assert.commandWorked(st.s.adminCommand({
            shardCollection: collNss,
            key: {hostId: 'hashed'},
            timeseries: {timeField: 'time', metaField: 'hostId'}
        }));

        validateCollectionSharded({
            shardKey: {meta: 'hashed'},
            timeSeriesParams: {timeField: 'time', metaField: 'hostId'}
        });

        validateRawIndexBackingShardKey({expectedKey: {'meta': 'hashed'}});

        assert.eq(coll.find().itcount(), 2);  // Validate count after sharding.
        let insertCount = timeseriesInsert(coll);
        assert.eq(coll.find().itcount(), insertCount + 2);
        coll.drop();

        if (collectionExists) {
            assert.commandWorked(sDB.createCollection(
                collName, {timeseries: {timeField: 'time', metaField: 'hostId'}}));
        }
        assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));

        // Sharding key with hashed meta field and time field.
        assert.commandWorked(st.s.adminCommand({
            shardCollection: collNss,
            key: {hostId: 'hashed', time: 1},
            timeseries: {timeField: 'time', metaField: 'hostId'}
        }));

        validateRawIndexBackingShardKey(
            {expectedKey: {'meta': 'hashed', 'control.min.time': 1, 'control.max.time': 1}});

        coll = sDB.getCollection(collName);
        assert.eq(coll.find().itcount(), 0);
        insertCount = timeseriesInsert(coll);
        assert.eq(coll.find().itcount(), insertCount);
        coll.drop();
    })();

    // Test that invalid shard keys fail.
    (function invalidShardKeyPatterns() {
        if (collectionExists) {
            assert.commandWorked(sDB.createCollection(
                collName, {timeseries: {timeField: 'time', metaField: 'hostId'}}));
        }

        // No other fields, including _id, are allowed in the shard key pattern
        assert.commandFailedWithCode(st.s.adminCommand({
            shardCollection: collNss,
            key: {_id: 1},
            timeseries: {timeField: 'time', metaField: 'hostId'},
        }),
                                     [5914001]);

        assert.commandFailedWithCode(st.s.adminCommand({
            shardCollection: collNss,
            key: {_id: 1, time: 1},
            timeseries: {timeField: 'time', metaField: 'hostId'},
        }),
                                     [5914001]);

        assert.commandFailedWithCode(st.s.adminCommand({
            shardCollection: collNss,
            key: {_id: 1, hostId: 1},
            timeseries: {timeField: 'time', metaField: 'hostId'},
        }),
                                     [5914001]);

        assert.commandFailedWithCode(st.s.adminCommand({
            shardCollection: collNss,
            key: {a: 1},
            timeseries: {timeField: 'time', metaField: 'hostId'},
        }),
                                     [5914001]);

        // Shared key where time is not the last field in shard key should fail.
        assert.commandFailedWithCode(st.s.adminCommand({
            shardCollection: collNss,
            key: {time: 1, hostId: 1},
            timeseries: {timeField: 'time', metaField: 'hostId'}
        }),
                                     [5914000]);
        assert(sDB.getCollection(collName).drop());
    })();

    (function noMetaFieldTimeseries() {
        if (collectionExists) {
            assert.commandWorked(sDB.createCollection(collName, {timeseries: {timeField: 'time'}}));
        }

        assert.commandFailedWithCode(st.s.adminCommand({
            shardCollection: collNss,
            key: {_id: 1},
            timeseries: {timeField: 'time'},
        }),
                                     [5914001]);

        assert.commandFailedWithCode(st.s.adminCommand({
            shardCollection: collNss,
            key: {a: 1},
            timeseries: {timeField: 'time'},
        }),
                                     [5914001]);

        assert.commandWorked(st.s.adminCommand(
            {shardCollection: collNss, key: {time: 1}, timeseries: {timeField: 'time'}}));

        validateRawIndexBackingShardKey(
            {expectedKey: {'control.min.time': 1, 'control.max.time': 1}});

        assert(sDB.getCollection(collName).drop());
    })();
}

runShardKeyPatternValidation(true);
runShardKeyPatternValidation(false);

// TODO SERVER-101784 remove this check once only viewless timeseries exist.
// Cannot shard the system.buckets namespace.
assert.commandFailedWithCode(st.s.adminCommand({
    shardCollection: `${dbName}.${getTimeseriesBucketsColl(collName)}`,
    key: {time: 1},
}),
                             5731501);

st.stop();
