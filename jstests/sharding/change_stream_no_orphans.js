/**
 * Test that write operations on orphaned documents (1) do not show up unexpected events in change
 * streams and (2) have no effect on the persisted data.
 *
 * The behavior is tested in the following scenarios:
 *   - Direct update/delete (from shard) to orphaned document
 *   - Distributed update/delete (from router) on a single document
 *   - Distributed update/delete (from router) to multi-documents
 *   - Distributed update/delete (from router) via transaction on multi-documents
 *
 * @tags: [
 *   requires_fcv_52,
 *   featureFlagNoChangeStreamEventsDueToOrphans,
 * ]
 */
(function() {
'use strict';

load('jstests/libs/fail_point_util.js');  // For configureFailPoint

const dbName = 'test';
const collName = 'foo';
const collNS = dbName + '.' + collName;

// Enable explicitly the periodic no-op writer to allow the router to process change stream events
// coming from all shards. This is enabled for production clusters by default.
const st = new ShardingTest({
    mongos: 1,
    config: 1,
    shards: 2,
    rs: {nodes: 1, setParameter: {writePeriodicNoops: true}},
    other: {enableBalancer: false}
});

// Suspend the range deletion on the first shard to force the orphaned documents to stay here after
// the chunks have been moved to the second shard.
let suspendRangeDeletionShard0 = configureFailPoint(st.shard0, 'suspendRangeDeletion');

// Create a shard collection with documents having both a key field and a non-key field.
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: collNS, key: {_id: 1}}));
const coll = st.s.getCollection(collNS);
assert.commandWorked(coll.insert({_id: 0, name: 'matt', age: 20}));
assert.commandWorked(coll.insert({_id: 1, name: 'john', age: 25}));
assert.commandWorked(coll.insert({_id: 2, name: 'robert', age: 30}));
assert.commandWorked(coll.insert({_id: 3, name: 'robert', age: 35}));
assert.commandWorked(coll.insert({_id: 4, name: 'james', age: 40}));
assert.commandWorked(coll.insert({_id: 5, name: 'james', age: 45}));

// Move the chunk to the second shard leaving orphaned documents on the first shard.
assert.commandWorked(st.s.adminCommand({split: collNS, middle: {_id: 0}}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: collNS, find: {_id: 0}, to: st.shard1.shardName}));

// Setup a change stream on the collection to receive real-time events on any data changes.
const changeStream = coll.watch([]);

jsTest.log('A direct update to a shard on an orphaned document generates an update event');
{
    // Send a direct update to first shard on an orphaned document.
    assert.commandWorked(st.shard0.getCollection(collNS).update({name: 'matt'}, {$set: {age: 21}}));

    // The orphaned document is actually updated and an update event is notified.
    assert.soon(() => changeStream.hasNext(), 'An update event is expected');
    assert.eq(changeStream.next().operationType, 'update');
    assert(!changeStream.hasNext());

    // The orphaned document on first shard has been touched.
    assert.eq(21, st.shard0.getCollection(collNS).findOne({_id: 0}).age);
}

jsTest.log('A direct delete to a shard on an orphaned document generates an update event');
{
    // Send a direct delete to first shard on an orphaned document.
    assert.commandWorked(st.shard0.getCollection(collNS).remove({name: 'matt'}));

    // The orphaned document is actually deleted and a delete event is notified.
    assert.soon(() => changeStream.hasNext(), 'A delete event is expected');
    assert.eq(changeStream.next().operationType, 'delete');
    assert(!changeStream.hasNext());

    // The orphaned document on first shard has been touched.
    assert.eq(null, st.shard0.getCollection(collNS).findOne({_id: 0}));
}

jsTest.log('A distributed update on a single document generates an update event');
{
    // Send a distributed update (query on non-key field) on a single document to all the shards.
    assert.commandWorked(coll.update({name: 'john'}, {$set: {age: 26}}, {multi: true}));

    // The document is hosted by the second shard and the update event is notified. The first shard
    // still hosts the orphaned document so no additional event must be notified.
    assert.soon(() => changeStream.hasNext(), 'An update event is expected');
    assert.eq(changeStream.next().operationType, 'update');
    assert(!changeStream.hasNext());

    // The orphaned document on first shard has not been touched, unlike the non-orphaned one on the
    // second shard.
    assert.eq(25, st.shard0.getCollection(collNS).findOne({_id: 1}).age);
    assert.eq(26, st.shard1.getCollection(collNS).findOne({_id: 1}).age);
}

jsTest.log('A distributed delete on a single document generates a delete event');
{
    // Send a distributed delete (query on non-key field) on a single document to all the shards.
    assert.commandWorked(coll.remove({name: 'john'}));

    // The document is hosted by the second shard and the delete event is notified. The first shard
    // still hosts the orphaned document so no additional event must be notified.
    assert.soon(() => changeStream.hasNext(), 'A delete event is expected');
    assert.eq(changeStream.next().operationType, 'delete');
    assert(!changeStream.hasNext());

    // The orphaned document on first shard has not been touched, unlike the non-orphaned one on the
    // second shard.
    assert.neq(null, st.shard0.getCollection(collNS).findOne({_id: 1}));
    assert.eq(null, st.shard1.getCollection(collNS).findOne({_id: 1}));
}

jsTest.log('A distributed update on multi-documents generates more update events');
{
    // Send a distributed update (query on non-key field) on two documents to all the shards.
    assert.commandWorked(coll.update({name: 'robert'}, {$set: {age: 36}}, {multi: true}));

    // The documents are hosted by the second shard and two delete events are notified. The first
    // shard still hosts the orphaned documents so no additional event must be notified.
    assert.soon(() => changeStream.hasNext(), 'A first update event is expected');
    assert.eq(changeStream.next().operationType, 'update');
    assert.soon(() => changeStream.hasNext(), 'A second update event is expected');
    assert.eq(changeStream.next().operationType, 'update');
    assert(!changeStream.hasNext());

    // The orphaned documents on first shard have not been touched, unlike the non-orphaned ones on
    // the second shard.
    assert.eq(30, st.shard0.getCollection(collNS).findOne({_id: 2}).age);
    assert.eq(35, st.shard0.getCollection(collNS).findOne({_id: 3}).age);
    assert.eq(36, st.shard1.getCollection(collNS).findOne({_id: 2}).age);
    assert.eq(36, st.shard1.getCollection(collNS).findOne({_id: 3}).age);
}

jsTest.log('A distributed delete on multi-documents generates more delete events');
{
    // Send a distributed delete (query on non-key field) on two documents to all the shards.
    assert.commandWorked(coll.remove({name: 'robert'}));

    // The documents are hosted by the second shard and two delete events are notified. The first
    // shard still hosts the orphaned documents so no additional event must be notified.
    assert.soon(() => changeStream.hasNext(), 'A first delete event is expected');
    assert.eq(changeStream.next().operationType, 'delete');
    assert.soon(() => changeStream.hasNext(), 'A second delete event is expected');
    assert.eq(changeStream.next().operationType, 'delete');
    assert(!changeStream.hasNext());

    // The orphaned documents on first shard have not been touched, unlike the non-orphaned ones on
    // the second shard.
    assert.neq(null, st.shard0.getCollection(collNS).findOne({_id: 2}));
    assert.neq(null, st.shard0.getCollection(collNS).findOne({_id: 3}));
    assert.eq(null, st.shard1.getCollection(collNS).findOne({_id: 2}));
    assert.eq(null, st.shard1.getCollection(collNS).findOne({_id: 3}));
}

jsTest.log('A distributed update via transaction on multi-documents generates more update events');
{
    // Send a distributed update (query on non-key field) via transaction on two documents to all
    // the shards.
    const session = st.s.startSession();
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);
    session.startTransaction();
    assert.commandWorked(sessionColl.update({name: 'james'}, {$set: {age: 46}}, {multi: true}));
    assert.commandWorked(session.commitTransaction_forTesting());
    session.endSession();

    // The documents are hosted by the second shard and two delete events are notified. The first
    // shard still hosts the orphaned documents so no additional event must be notified.
    assert.soon(() => changeStream.hasNext(), 'A first update event is expected');
    assert.eq(changeStream.next().operationType, 'update');
    assert.soon(() => changeStream.hasNext(), 'A second update event is expected');
    assert.eq(changeStream.next().operationType, 'update');
    assert(!changeStream.hasNext());

    // The orphaned documents on first shard have not been touched, unlike the non-orphaned ones on
    // the second shard.
    assert.eq(40, st.shard0.getCollection(collNS).findOne({_id: 4}).age);
    assert.eq(45, st.shard0.getCollection(collNS).findOne({_id: 5}).age);
    assert.eq(46, st.shard1.getCollection(collNS).findOne({_id: 4}).age);
    assert.eq(46, st.shard1.getCollection(collNS).findOne({_id: 5}).age);
}

jsTest.log('A distributed delete via transaction on multi-documents generates more delete events');
{
    // Send a distributed delete (query on non-key field) via transaction on two documents to all
    // the shards.
    const session = st.s.startSession();
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);
    session.startTransaction();
    assert.commandWorked(sessionColl.remove({name: 'james'}));
    assert.commandWorked(session.commitTransaction_forTesting());
    session.endSession();

    // The documents are hosted by the second shard and two delete events are notified. The first
    // shard still hosts the orphaned documents so no additional event must be notified.
    assert.soon(() => changeStream.hasNext(), 'A first delete event is expected');
    assert.eq(changeStream.next().operationType, 'delete');
    assert.soon(() => changeStream.hasNext(), 'A second delete event is expected');
    assert.eq(changeStream.next().operationType, 'delete');
    assert(!changeStream.hasNext());

    // The orphaned documents on first shard have not been touched, unlike the non-orphaned ones on
    // the second shard.
    assert.neq(null, st.shard0.getCollection(collNS).findOne({_id: 4}));
    assert.neq(null, st.shard0.getCollection(collNS).findOne({_id: 5}));
    assert.eq(null, st.shard1.getCollection(collNS).findOne({_id: 4}));
    assert.eq(null, st.shard1.getCollection(collNS).findOne({_id: 5}));
}

jsTest.log('The collection drop generates a drop event');
{
    coll.drop();

    // Essentially, this verifies that the operation before dropping the collection did not notify
    // additional and unexpected events.
    assert.soon(() => changeStream.hasNext(), 'A drop event is expected');
    assert.eq(changeStream.next().operationType, 'drop');
}

suspendRangeDeletionShard0.off();

st.stop();
})();
