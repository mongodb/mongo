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
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const docCount = 50;

const runTestCase = function (fn, isSharded = false) {
    if (!isSharded) {
        const replTest = new ReplSetTest({
            nodes: 1,
            nodeOptions: {setParameter: {ttlMonitorSleepSecs: 1}, slowms: 0},
        });
        replTest.startSet();
        replTest.initiate();

        fn(replTest.getPrimary());

        replTest.stopSet();
    } else {
        const st = new ShardingTest({
            shards: 1,
            mongos: 1,
            config: 1,
            other: {
                rsOptions: {setParameter: {ttlMonitorSleepSecs: 1}, slowms: 0},
            },
        });
        const conn = st.s0;

        fn(conn);

        st.stop();
    }
};

const countIndexKeysDeleted = function () {
    const logs = rawMongoProgramOutput('"numKeysDeleted"');
    const indexKeyDeletedLogs = [...logs.matchAll(/"numKeysDeleted":(\d*)/g)];
    const keysDeleted = indexKeyDeletedLogs.reduce((acc, curr) => acc + parseInt(curr[1]), 0);
    clearRawMongoProgramOutput();
    return keysDeleted;
};

const disableTTLBatchDeletes = function (conn) {
    // Disable TTL batch deletion.
    assert.commandWorked(conn.getDB("admin").runCommand({setParameter: 1, ttlMonitorBatchDeletes: false}));
};

const triggerIndexScanTTL = function (db, doShardCollection = false) {
    const coll = db.ttl_coll;
    coll.drop();

    if (doShardCollection) {
        assert.commandWorked(db.adminCommand({enableSharding: db.getName()}));
        assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {_id: "hashed"}}));
    }

    // Insert 50 docs with timestamp 'now - 24h'.
    const now = new Date().getTime();
    const past = new Date(now - 3600 * 1000 * 24);
    for (let i = 0; i < docCount; i++) {
        assert.commandWorked(db.runCommand({insert: "ttl_coll", documents: [{x: past}]}));
    }

    assert.eq(coll.find().itcount(), docCount);

    // Create TTL index: expire docs older than 20000 seconds (~5.5h).
    coll.createIndex({x: 1}, {expireAfterSeconds: 20000});

    assert.soon(function () {
        return coll.find().itcount() == 0;
    }, "TTL index on x didn't delete");
    assert.eq(docCount * (doShardCollection ? 3 : 2), countIndexKeysDeleted());
};

const testTTLDeleteWithIndexScanBatched = function (conn) {
    const db = conn.getDB("test");
    triggerIndexScanTTL(db);

    // Verify batchedDeletes status to verify ttl deletions have been batched.
    assert.eq(db.serverStatus()["batchedDeletes"]["docs"], docCount);
};

const testTTLDeleteWithIndexScanDocByDoc = function (conn) {
    const db = conn.getDB("test");

    disableTTLBatchDeletes(conn);

    triggerIndexScanTTL(db);

    // Verify batchedDeletes status to verify ttl deletions batch doc count is 0.
    assert.eq(db.serverStatus()["batchedDeletes"]["docs"], 0);
};

const triggerCollectionScanTTL = function (testDB, doShardCollection = false) {
    const timeFieldName = "time";
    const metaFieldName = "host";
    const expireAfterSeconds = 5;
    // Default maximum range of time for a bucket.
    const defaultBucketMaxRange = 3600;

    const coll = testDB.getCollection("ts");
    assert.commandWorked(
        testDB.createCollection(coll.getName(), {
            timeseries: {
                timeField: timeFieldName,
                metaField: metaFieldName,
            },
            expireAfterSeconds: expireAfterSeconds,
        }),
    );

    if (doShardCollection) {
        assert.commandWorked(coll.createIndex({[metaFieldName]: 1}));
        assert.commandWorked(testDB.adminCommand({shardCollection: coll.getFullName(), key: {[metaFieldName]: 1}}));
    }

    const maxTime = new Date(new Date().getTime() - 1000 * defaultBucketMaxRange);
    const minTime = new Date(maxTime.getTime() - 1000 * 5 * 60);
    // Insert a single measurement to create a single bucket document. Inserting multiple measurements
    // creates some flakiness in terms of how many buckets are ultimately created for this collection
    // (i.e, depending on whether the TTL Monitor run before we've inserted all of our measurements
    // which would result in two buckets being created and deleted, or whether we insert all measurements
    // into a single bucket which then gets deleted).
    const time = new Date(minTime.getTime());
    assert.commandWorked(coll.insert({[timeFieldName]: time, [metaFieldName]: "localhost"}));

    assert.soon(function () {
        return coll.find().itcount() == 0;
    }, "TTL index on the cluster key didn't delete");

    assert.eq(0, coll.find().itcount());
    assert.eq(0, getTimeseriesCollForRawOps(testDB, coll).find().rawData().itcount());
    assert.eq(doShardCollection ? 2 : 1, countIndexKeysDeleted());
};

const testTTLDeleteWithCollectionScanBatched = function (conn) {
    const testDB = conn.getDB("test");

    triggerCollectionScanTTL(testDB);

    // For time series the "docs" count is related to buckets instead of documents. Check that the
    // number of batches is greater than 0 instead.
    assert.gt(testDB.serverStatus()["batchedDeletes"]["batches"], 0);
};

const testTTLDeleteWithCollectionScanDocByDoc = function (conn) {
    const testDB = conn.getDB("test");

    disableTTLBatchDeletes(conn);

    triggerCollectionScanTTL(testDB);

    assert.eq(testDB.serverStatus()["batchedDeletes"]["batches"], 0);
};

const verifyTTLOnShardedCluster = function (conn, clustered = false) {
    const db = conn.getDB("test");
    if (clustered) {
        triggerCollectionScanTTL(db, true /* doShardCollection */);
    } else {
        triggerIndexScanTTL(db, true /* doShardCollection */);
    }

    // Verify batchedDeletes status to verify ttl deletions have been batched.
    let sum = 0;
    FixtureHelpers.mapOnEachShardNode({
        db: db,
        func: (db) => {
            sum += db.serverStatus()["batchedDeletes"]["docs"];
        },
    });

    if (clustered) {
        // For time series the "docs" count is related to buckets instead of documents. Check that
        // the number of batches is greater than 0 instead.
        assert.gte(sum, 0);
    } else {
        assert.eq(sum, docCount);
    }
};

const testTTLDeleteWithIndexScanBatchedOnShardedCollection = function (conn) {
    verifyTTLOnShardedCluster(conn, false);
};

const testTTLDeleteWithCollectionScanBatchedOnShardedCollection = function (conn) {
    verifyTTLOnShardedCluster(conn, true);
};

runTestCase(testTTLDeleteWithIndexScanBatched);
runTestCase(testTTLDeleteWithIndexScanDocByDoc);
runTestCase(testTTLDeleteWithCollectionScanBatched);
runTestCase(testTTLDeleteWithCollectionScanDocByDoc);
runTestCase(testTTLDeleteWithIndexScanBatchedOnShardedCollection, true);
runTestCase(testTTLDeleteWithCollectionScanBatchedOnShardedCollection, true);
