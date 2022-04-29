/**
 * Verify the behavior of TTL batched deletes and ttlMonitorBatchDeletes parameter.
 *
 * @tags: [
 *   requires_replication,
 *   requires_sharding,
 *   requires_timeseries,
 *   uses_ttl,
 * ]
 */
(function() {
'use strict';
load("jstests/libs/clustered_collections/clustered_collection_util.js");
load('jstests/replsets/rslib.js');
load("jstests/libs/fixture_helpers.js");  // For mapOnEachShardNode().

const docCount = 50;

const runTestCase = function(fn, isSharded = false) {
    if (!isSharded) {
        // TODO: Remove featureFlagBatchMultiDeletes after SERVER-55750.
        const replTest = new ReplSetTest({
            nodes: 1,
            nodeOptions:
                {setParameter: {ttlMonitorSleepSecs: 1, featureFlagBatchMultiDeletes: true}},
        });
        replTest.startSet();
        replTest.initiate();

        fn(replTest.getPrimary());

        replTest.stopSet();
    } else {
        // TODO: Remove featureFlagBatchMultiDeletes after SERVER-55750.
        const st = new ShardingTest({
            shards: 1,
            mongos: 1,
            config: 1,
            other: {
                shardOptions:
                    {setParameter: {ttlMonitorSleepSecs: 1, featureFlagBatchMultiDeletes: true}},
            }
        });
        const conn = st.s0;

        fn(conn);

        st.stop();
    }
};

const disableTTLBatchDeletes = function(conn) {
    // Disable TTL batch deletion.
    assert.commandWorked(
        conn.getDB('admin').runCommand({setParameter: 1, ttlMonitorBatchDeletes: false}));
};

const triggerIndexScanTTL = function(db, doShardCollection = false) {
    const coll = db.ttl_coll;
    coll.drop();

    if (doShardCollection) {
        assert.commandWorked(db.adminCommand({enableSharding: db.getName()}));
        assert.commandWorked(
            db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 'hashed'}}));
    }

    // Insert 50 docs with timestamp 'now - 24h'.
    const now = (new Date()).getTime();
    const past = new Date(now - (3600 * 1000 * 24));
    for (let i = 0; i < docCount; i++) {
        assert.commandWorked(db.runCommand({insert: 'ttl_coll', documents: [{x: past}]}));
    }

    assert.eq(coll.find().itcount(), docCount);

    // Create TTL index: expire docs older than 20000 seconds (~5.5h).
    coll.createIndex({x: 1}, {expireAfterSeconds: 20000});

    assert.soon(function() {
        return coll.find().itcount() == 0;
    }, 'TTL index on x didn\'t delete');
};

const testTTLDeleteWithIndexScanBatched = function(conn) {
    const db = conn.getDB('test');
    triggerIndexScanTTL(db);

    // Verify batchedDeletes status to verify ttl deletions have been batched.
    assert.eq(db.serverStatus()["batchedDeletes"]["docs"], docCount);
};

const testTTLDeleteWithIndexScanDocByDoc = function(conn) {
    const db = conn.getDB('test');

    disableTTLBatchDeletes(conn);

    triggerIndexScanTTL(db);

    // Verify batchedDeletes status to verify ttl deletions batch doc count is 0.
    assert.eq(db.serverStatus()["batchedDeletes"]["docs"], 0);
};

const triggerCollectionScanTTL = function(testDB) {
    const timeFieldName = 'time';
    const metaFieldName = 'host';
    const expireAfterSeconds = 5;
    // Default maximum range of time for a bucket.
    const defaultBucketMaxRange = 3600;

    const coll = testDB.getCollection('ts');
    const bucketsColl = testDB.getCollection('system.buckets.' + coll.getName());
    assert.commandWorked(testDB.createCollection(coll.getName(), {
        timeseries: {
            timeField: timeFieldName,
            metaField: metaFieldName,
        },
        expireAfterSeconds: expireAfterSeconds,
    }));

    const maxTime = new Date((new Date()).getTime() - (1000 * defaultBucketMaxRange));
    const minTime = new Date(maxTime.getTime() - (1000 * 5 * 60));
    for (let i = 0; i < docCount; i++) {
        const time = new Date(minTime.getTime() + i);
        assert.commandWorked(coll.insert({[timeFieldName]: time, [metaFieldName]: "localhost"}));
    }

    ClusteredCollectionUtil.waitForTTL(testDB);

    assert.eq(0, coll.find().itcount());
    assert.eq(0, bucketsColl.find().itcount());
};

const testTTLDeleteWithCollectionScanBatched = function(conn) {
    const testDB = conn.getDB('test');

    triggerCollectionScanTTL(testDB);

    // For time series the "docs" count is related to buckets instead of documents. Check that the
    // number of batches is greater than 0 instead.
    assert.gt(testDB.serverStatus()["batchedDeletes"]["batches"], 0);
};

const testTTLDeleteWithCollectionScanDocByDoc = function(conn) {
    const testDB = conn.getDB('test');

    disableTTLBatchDeletes(conn);

    triggerCollectionScanTTL(testDB);

    assert.eq(testDB.serverStatus()["batchedDeletes"]["batches"], 0);
};

const testTTLDeleteWithIndexScanBatchedExcludesShardedCollection = function(conn) {
    const db = conn.getDB('test');
    triggerIndexScanTTL(db, true);

    // Verify batchedDeletes status to verify ttl deletions have not been batched.
    var sum = 0;
    FixtureHelpers.mapOnEachShardNode({
        db: db,
        func: (db) => {
            sum += db.serverStatus()["batchedDeletes"]["docs"];
        }
    });

    assert.eq(sum, 0);
};

runTestCase(testTTLDeleteWithIndexScanBatched);
runTestCase(testTTLDeleteWithIndexScanDocByDoc);
runTestCase(testTTLDeleteWithCollectionScanBatched);
runTestCase(testTTLDeleteWithCollectionScanDocByDoc);
// Excluding sharded collections from batched deletes is a temporary measure.
// TODO: Remove this test after SERVER-64107 and SERVER-65644 are resolved.
runTestCase(testTTLDeleteWithIndexScanBatchedExcludesShardedCollection, true);
})();
