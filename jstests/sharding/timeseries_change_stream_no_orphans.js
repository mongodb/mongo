/**
 * Verifies that delete on orphaned buckets (1) do not show up unexpected events in change streams
 * and (2) have a certain behavior on the persisted data.
 *
 * The behavior is tested in the following scenarios:
 *   - Test case 1: Direct operations to shard on orphaned documents
 *   - Test case 2: Broadcasted operations (from router) on orphaned documents
 *
 * @tags: [
 *   # To avoid multiversion tests
 *   requires_fcv_70,
 *   # To avoid burn-in tests in in-memory build variants
 *   requires_persistence,
 *   featureFlagTimeseriesDeletesSupport,
 * ]
 */
(function() {
'use strict';

load('jstests/libs/fail_point_util.js');  // For configureFailPoint

// Asserts that there is no change stream event.
function assertNoChanges(csCursor) {
    function advanceAndGetToken() {
        assert(!csCursor.hasNext(), () => csCursor.next());
        return csCursor.getResumeToken();
    }
    const startToken = advanceAndGetToken();
    assert.soon(() => {
        const currentToken = advanceAndGetToken();
        return bsonWoCompare(currentToken, startToken) > 0;
    });
}

const dbName = "ts_change_stream_no_orphans";
const collName = 'ts';
const sysCollName = 'system.buckets.' + collName;
const collNS = dbName + '.' + collName;
const sysCollNS = dbName + '.' + sysCollName;
const timeseriesOpt = {
    timeField: "time",
    metaField: "tag",
};

function validateBucketsCollectionSharded({shardKey}) {
    const configColls = st.s.getDB('config').collections;
    const output = configColls
                       .find({
                           _id: sysCollNS,
                           key: shardKey,
                           timeseriesFields: {$exists: true},
                       })
                       .toArray();
    assert.eq(output.length, 1, configColls.find().toArray());
    assert.eq(output[0].timeseriesFields.timeField, timeseriesOpt.timeField, output[0]);
    assert.eq(output[0].timeseriesFields.metaField, timeseriesOpt.metaField, output[0]);
}

// Enables explicitly the periodic no-op writer to allow the router to process change stream events
// coming from all shards. This is enabled for production clusters by default.
const st = new ShardingTest({
    mongos: 1,
    config: 1,
    shards: 2,
    rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
    other: {enableBalancer: false}
});

// Suspends the range deletion on the first shard to force the orphaned buckets to stay at the
// first shard after the chunks have been moved to the second shard.
let suspendRangeDeletionShard0 = configureFailPoint(st.shard0, 'suspendRangeDeletion');

// Creates a shard collection with buckets having both a key field and a non-key field. The key
// is the metaField of the timeseries collection.
jsTest.log(`Shard a timeseries collection: ${collNS} with shard key: {tag: 1}`);
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
const db = st.s.getDB(dbName);
db[collName].drop();
assert.commandWorked(db.createCollection(collName, {timeseries: timeseriesOpt}));
assert.commandWorked(st.s.adminCommand({shardCollection: collNS, key: {tag: 1}}));
// The meta field name on the system.buckets collection is 'meta' and hence the shardKey is 'meta'.
validateBucketsCollectionSharded({shardKey: {meta: 1}});

const mongosColl = st.s.getCollection(collNS);
assert.commandWorked(mongosColl.insert({_id: 0, tag: -1, time: ISODate(), f: 20}));
assert.commandWorked(mongosColl.insert({_id: 1, tag: -1, time: ISODate(), f: 25}));
assert.commandWorked(mongosColl.insert({_id: 2, tag: 0, time: ISODate(), f: 30}));  // Test case 1
assert.commandWorked(mongosColl.insert({_id: 3, tag: 0, time: ISODate(), f: 30}));  // Test case 1
assert.commandWorked(mongosColl.insert({_id: 4, tag: 0, time: ISODate(), f: 40}));  // Test case 2
assert.commandWorked(mongosColl.insert({_id: 5, tag: 1, time: ISODate(), f: 45}));
assert.commandWorked(mongosColl.insert({_id: 6, tag: 2, time: ISODate(), f: 50}));  // Test case 2
assert.commandWorked(mongosColl.insert({_id: 7, tag: 2, time: ISODate(), f: 50}));  // Test case 2
assert.commandWorked(mongosColl.insert({_id: 8, tag: 3, time: ISODate(), f: 60}));  // Test case 1

// Moves the chunk to the second shard leaving orphaned buckets on the first shard. The orphaned
// buckets are not deleted because the range deletion is suspended and they are buckets with tag =
// [0, 3].
//
// Note: system admin commands such as 'split' and 'moveChunk' should run on the 'system.buckets'
// collection unlike the 'shardCollection' command.
assert.commandWorked(st.s.adminCommand({split: sysCollNS, middle: {meta: 0}}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: sysCollNS, find: {meta: 0}, to: st.shard1.shardName}));

// Sets up a change stream on the mongos database to receive real-time events on any data changes.
//
// Note: the change stream is on the database because watching the change stream events on the
// system.buckets collection is not allowed.
const mongosDbChangeStream = db.watch([], {showSystemEvents: true});

////////////////////////////////////////////////////////////////////////////////////////////////////
// Test case 1: Direct operations to shard on orphaned buckets
////////////////////////////////////////////////////////////////////////////////////////////////////

(function testDirectDeleteToShardOnOrphanedBuckets() {
    jsTest.log('A direct delete to a shard of an orphaned bucket does not generate a delete event');

    // Sends a direct delete to first shard on a measurement on an orphaned bucket.
    assert.commandWorked(st.shard0.getCollection(collNS).remove({f: 60}));

    // No event is generated on the database change stream.
    assertNoChanges(mongosDbChangeStream);

    // The entire orphaned bucket on the first shard has been removed with meta == 3 because the
    // measurement with {f: 60} was the only one in the bucket.
    assert.eq(null, st.shard0.getCollection(sysCollNS).findOne({meta: 3}));

    // But the measurement with {f: 60} still exists on the second shard and show up in the
    // cluster find.
    assert.eq(1, mongosColl.find({f: 60}).itcount(), mongosColl.find().toArray());
})();

(function testDirectMultiDeleteToShardOnOrphanedBuckets() {
    jsTest.log('A direct delete to a shard of multi-documents does not generate delete events');

    // Sends a direct delete to the first shard on measurements on an orphaned bucket.
    assert.commandWorked(st.shard0.getCollection(collNS).remove({f: 30}));

    // No event is generated on the database change stream.
    assertNoChanges(mongosDbChangeStream);

    // The orphaned bucket on the first shard have been updated since two measurements ({f: 30})
    // has been removed from the bucket and only the measurement with {_id: 4, f: 40} stays in
    // the bucket.
    const actualBucket = st.shard0.getCollection(sysCollNS).findOne({meta: 0});
    assert.eq(0, actualBucket.meta, actualBucket);
    assert.eq(4, actualBucket.control.min._id, actualBucket);
    assert.eq(4, actualBucket.control.max._id, actualBucket);
    assert.eq(40, actualBucket.control.min.f, actualBucket);
    assert.eq(40, actualBucket.control.max.f, actualBucket);
    assert.eq(4, actualBucket.data._id[0], actualBucket);
    assert.eq(40, actualBucket.data.f[0], actualBucket);

    // But the two measurements with {f: 30} still exists on the second shard and show up in the
    // cluster find.
    assert.eq(2, mongosColl.find({f: 30}).itcount(), mongosColl.find().toArray());
})();

////////////////////////////////////////////////////////////////////////////////////////////////////
// Test case 2: Broadcasted operations (from router) on orphaned buckets
////////////////////////////////////////////////////////////////////////////////////////////////////

(function testBroadcastedDeleteOnOrphanedBuckets() {
    jsTest.log(
        'A broadcasted delete of a single measurement generates a replace event of a bucket');

    // Sends a broadcasted delete (query on non-shardkey field) on a single measurement to all the
    // shards.
    assert.commandWorked(mongosColl.remove({_id: 4}));

    // The document is hosted by the second shard and the replace event is notified because a single
    // measurement is deleted from the containing bucket. The first shard still hosts the orphaned
    // document but no additional event must be notified.
    assert.soon(() => mongosDbChangeStream.hasNext(), 'A replace event of a bucket is expected');
    const change = mongosDbChangeStream.next();
    assert.eq(change.operationType, 'replace', change);
    assertNoChanges(mongosDbChangeStream);

    // The orphaned bucket on the first shard have not been updated, unlike the mongos collection.
    assert.eq(null, mongosColl.findOne({_id: 4}), mongosColl.find().toArray());
    const shard0Bucket = st.shard0.getCollection(sysCollNS).findOne({meta: 0});
    assert.eq(0, shard0Bucket.meta, shard0Bucket);
    assert.eq(4, shard0Bucket.control.min._id, shard0Bucket);
    assert.eq(4, shard0Bucket.control.max._id, shard0Bucket);
    assert.eq(40, shard0Bucket.control.min.f, shard0Bucket);
    assert.eq(40, shard0Bucket.control.max.f, shard0Bucket);
    assert.eq(4, shard0Bucket.data._id[0], shard0Bucket);
    assert.eq(40, shard0Bucket.data.f[0], shard0Bucket);
})();

(function testBroadcastedMultiDeleteOnOrphanedBuckets() {
    jsTest.log('A broadcasted delete of multi-documents generates a delete event of a bucket');

    // Sends a broadcasted delete (query on non-shardkey field) on two documents to all the shards.
    assert.commandWorked(mongosColl.remove({f: 50}));

    // The documents are hosted by the second shard and a bucket delete event is notified because
    // the all measurements are deleted from the bucket. The first shard still hosts the orphaned
    // documents but no additional event must be notified.
    assert.soon(() => mongosDbChangeStream.hasNext(), 'A delete event of a bucket is expected');
    const change = mongosDbChangeStream.next();
    assert.eq(change.operationType, 'delete', change);
    assertNoChanges(mongosDbChangeStream);

    // The orphaned bucket on first shard have not been removed, unlike the mongos collection.
    assert.eq(null, mongosColl.findOne({_id: 6}), mongosColl.find().toArray());
    assert.eq(null, mongosColl.findOne({_id: 7}), mongosColl.find().toArray());
    const shard0Bucket = st.shard0.getCollection(sysCollNS).findOne({meta: 2});
    assert.eq(2, shard0Bucket.meta, shard0Bucket);
    assert.eq(6, shard0Bucket.control.min._id, shard0Bucket);
    assert.eq(7, shard0Bucket.control.max._id, shard0Bucket);
    assert.eq(50, shard0Bucket.control.min.f, shard0Bucket);
    assert.eq(50, shard0Bucket.control.max.f, shard0Bucket);
    assert.eq(6, shard0Bucket.data._id[0], shard0Bucket);
    assert.eq(50, shard0Bucket.data.f[0], shard0Bucket);
})();

suspendRangeDeletionShard0.off();

st.stop();
})();
