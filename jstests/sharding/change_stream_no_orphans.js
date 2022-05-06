/**
 * Verify that write operations on orphaned documents (1) do not show up unexpected events in change
 * streams and (2) have no effect on the persisted data.
 *
 * The behavior is tested in the following scenarios:
 *   - Direct operations to shard on orphaned documents
 *   - Broadcasted operations (from router) on orphaned documents
 *   - Transaction from router updating both orphaned and non-orphaned documents
 *   - Transaction to shard updating both orphaned and non-orphaned documents
 *   - Batched deletes from router and to shard
 *
 * @tags: [
 *   requires_fcv_53,
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
assert.commandWorked(coll.insert({_id: -2, name: 'emma', age: 20}));
assert.commandWorked(coll.insert({_id: -1, name: 'olivia', age: 25}));
assert.commandWorked(coll.insert({_id: 0, name: 'matt', age: 30}));
assert.commandWorked(coll.insert({_id: 1, name: 'john', age: 35}));
assert.commandWorked(coll.insert({_id: 2, name: 'robert', age: 40}));
assert.commandWorked(coll.insert({_id: 3, name: 'robert', age: 45}));
assert.commandWorked(coll.insert({_id: 4, name: 'james', age: 50}));
assert.commandWorked(coll.insert({_id: 5, name: 'liam', age: 55}));

// Move the chunk to the second shard leaving orphaned documents on the first shard.
assert.commandWorked(st.s.adminCommand({split: collNS, middle: {_id: 0}}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: collNS, find: {_id: 0}, to: st.shard1.shardName}));

// Setup a change stream on the collection to receive real-time events on any data changes.
const changeStream = coll.watch([]);

////////////////////////////////////////////////////////////////////////////////////////////////////
// Direct operations to shard on orphaned documents
////////////////////////////////////////////////////////////////////////////////////////////////////

jsTest.log('A direct insert to a shard of an orphaned document does not generate an insert event');
{
    // Direct insert to first shard of an orphaned document.
    assert.commandWorked(st.shard0.getCollection(collNS).insert({_id: 6, name: 'ken', age: 60}));

    // No event is notified.
    assert(!changeStream.hasNext());

    // The orphaned document on first shard has been inserted.
    assert.neq(null, st.shard0.getCollection(collNS).findOne({_id: 6}));
}

jsTest.log('A direct update to a shard of an orphaned document does not generate an update event');
{
    // Send a direct update to first shard on an orphaned document.
    assert.commandWorked(st.shard0.getCollection(collNS).update({name: 'matt'}, {$set: {age: 31}}));

    // No change stream event is generated.
    assert(!changeStream.hasNext());

    // The orphaned document on first shard has been updated.
    assert.eq(31, st.shard0.getCollection(collNS).findOne({_id: 0}).age);
}

jsTest.log('A direct delete to a shard of an orphaned document does generate an update event');
{
    // Send a direct delete to first shard on an orphaned document.
    assert.commandWorked(st.shard0.getCollection(collNS).remove({name: 'matt'}));

    // No change stream event is generated.
    assert(!changeStream.hasNext());

    // The orphaned document on first shard has been removed.
    assert.eq(null, st.shard0.getCollection(collNS).findOne({_id: 0}));
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Broadcasted operations (from router) on orphaned documents
////////////////////////////////////////////////////////////////////////////////////////////////////

jsTest.log('A broadcasted update of a single document generates an update event');
{
    // Send a broadcasted update (query on non-key field) on a single document to all the shards.
    assert.commandWorked(coll.update({name: 'john'}, {$set: {age: 36}}, {multi: true}));

    // The document is hosted by the second shard and the update event is notified. The first shard
    // still hosts the orphaned document so no additional event must be notified.
    assert.soon(() => changeStream.hasNext(), 'An update event is expected');
    assert.eq(changeStream.next().operationType, 'update');
    assert(!changeStream.hasNext());

    // The orphaned document on first shard has not been updated, unlike the non-orphaned one on the
    // second shard.
    assert.eq(35, st.shard0.getCollection(collNS).findOne({_id: 1}).age);
    assert.eq(36, st.shard1.getCollection(collNS).findOne({_id: 1}).age);
}

jsTest.log('A broadcasted delete of a single document generates a delete event');
{
    // Send a broadcasted delete (query on non-key field) on a single document to all the shards.
    assert.commandWorked(coll.remove({name: 'john'}));

    // The document is hosted by the second shard and the delete event is notified. The first shard
    // still hosts the orphaned document so no additional event must be notified.
    assert.soon(() => changeStream.hasNext(), 'A delete event is expected');
    assert.eq(changeStream.next().operationType, 'delete');
    assert(!changeStream.hasNext());

    // The orphaned document on first shard has not been removed, unlike the non-orphaned one on the
    // second shard.
    assert.neq(null, st.shard0.getCollection(collNS).findOne({_id: 1}));
    assert.eq(null, st.shard1.getCollection(collNS).findOne({_id: 1}));
}

jsTest.log('A broadcasted update of multi-documents generates more update events');
{
    // Send a broadcasted update (query on non-key field) on two documents to all the shards.
    assert.commandWorked(coll.update({name: 'robert'}, {$set: {age: 41}}, {multi: true}));

    // The documents are hosted by the second shard and two delete events are notified. The first
    // shard still hosts the orphaned documents so no additional event must be notified.
    assert.soon(() => changeStream.hasNext(), 'A first update event is expected');
    assert.eq(changeStream.next().operationType, 'update');
    assert.soon(() => changeStream.hasNext(), 'A second update event is expected');
    assert.eq(changeStream.next().operationType, 'update');
    assert(!changeStream.hasNext());

    // The orphaned documents on first shard have not been updated, unlike the non-orphaned ones on
    // the second shard.
    assert.eq(40, st.shard0.getCollection(collNS).findOne({_id: 2}).age);
    assert.eq(45, st.shard0.getCollection(collNS).findOne({_id: 3}).age);
    assert.eq(41, st.shard1.getCollection(collNS).findOne({_id: 2}).age);
    assert.eq(41, st.shard1.getCollection(collNS).findOne({_id: 3}).age);
}

jsTest.log('A broadcasted delete of multi-documents generates more delete events');
{
    // Send a broadcasted delete (query on non-key field) on two documents to all the shards.
    assert.commandWorked(coll.remove({name: 'robert'}));

    // The documents are hosted by the second shard and two delete events are notified. The first
    // shard still hosts the orphaned documents so no additional event must be notified.
    assert.soon(() => changeStream.hasNext(), 'A first delete event is expected');
    assert.eq(changeStream.next().operationType, 'delete');
    assert.soon(() => changeStream.hasNext(), 'A second delete event is expected');
    assert.eq(changeStream.next().operationType, 'delete');
    assert(!changeStream.hasNext());

    // The orphaned documents on first shard have not been removed, unlike the non-orphaned ones on
    // the second shard.
    assert.neq(null, st.shard0.getCollection(collNS).findOne({_id: 2}));
    assert.neq(null, st.shard0.getCollection(collNS).findOne({_id: 3}));
    assert.eq(null, st.shard1.getCollection(collNS).findOne({_id: 2}));
    assert.eq(null, st.shard1.getCollection(collNS).findOne({_id: 3}));
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Transaction from router updating both orphaned and non-orphaned documents
////////////////////////////////////////////////////////////////////////////////////////////////////

jsTest.log('Broadcasted updates (via a transaction through the router) of both orphaned and ' +
           'non-orphaned documents generate events only for operations on non-orphaned documents');
{
    // Send a broadcasted transaction to the router updating both orphaned and non-orphaned
    // documents.
    const session = st.s.startSession();
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);
    session.startTransaction();
    assert.commandWorked(sessionColl.update({name: 'olivia'}, {$set: {age: 26}}, {multi: true}));
    assert.commandWorked(sessionColl.update({name: 'james'}, {$set: {age: 51}}, {multi: true}));
    assert.commandWorked(session.commitTransaction_forTesting());
    session.endSession();

    // The primary shard hosts orphaned (james) and non-orphaned (olivia) documents, whereas the
    // second shard hosts a non-orphaned document (james). Consequently, two update events are
    // notified.
    assert.soon(() => changeStream.hasNext(), 'A first update event is expected');
    assert.eq(changeStream.next().operationType, 'update');
    assert.soon(() => changeStream.hasNext(), 'A second update event is expected');
    assert.eq(changeStream.next().operationType, 'update');
    assert(!changeStream.hasNext());

    // The orphaned document on first shard (james) has not been updated, unlike the non-orphaned
    // ones on both primary and second shards (olivia and james).
    assert.eq(26, st.shard0.getCollection(collNS).findOne({_id: -1}).age);
    assert.eq(50, st.shard0.getCollection(collNS).findOne({_id: 4}).age);
    assert.eq(51, st.shard1.getCollection(collNS).findOne({_id: 4}).age);
}

jsTest.log('Broadcasted deletes (via a transaction through the router) of both orphaned and ' +
           'non-orphaned documents generate events only for operations on non-orphaned documents');
{
    // Send a broadcasted transaction to the router deleting both orphaned and non-orphaned
    // documents.
    const session = st.s.startSession();
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);
    session.startTransaction();
    assert.commandWorked(sessionColl.remove({name: 'olivia'}));
    assert.commandWorked(sessionColl.remove({name: 'james'}));
    assert.commandWorked(session.commitTransaction_forTesting());
    session.endSession();

    // The primary shard hosts orphaned (james) and non-orphaned (olivia) documents, whereas the
    // second shard hosts a non-orphaned document (james). Consequently, two delete events are
    // notified.
    assert.soon(() => changeStream.hasNext(), 'A first delete event is expected');
    assert.eq(changeStream.next().operationType, 'delete');
    assert.soon(() => changeStream.hasNext(), 'A second delete event is expected');
    assert.eq(changeStream.next().operationType, 'delete');
    assert(!changeStream.hasNext());

    // The orphaned document on first shard (james) has not been removed, unlike the non-orphaned
    // ones on both primary and second shards (olivia and james).
    assert.eq(null, st.shard0.getCollection(collNS).findOne({_id: -1}));
    assert.neq(null, st.shard0.getCollection(collNS).findOne({_id: 4}));
    assert.eq(null, st.shard1.getCollection(collNS).findOne({_id: 4}));
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Transaction to shard updating both orphaned and non-orphaned documents
////////////////////////////////////////////////////////////////////////////////////////////////////

jsTest.log('Direct updates (via a transaction to a shard) of both orphaned and non-orphaned' +
           'documents generate events only for operations on non-orphaned documents');
{
    // Send a direct transaction to a shard updating both orphaned and non-orphaned documents.
    const session = st.rs0.getPrimary().getDB(dbName).getMongo().startSession();
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);
    session.startTransaction();
    assert.commandWorked(sessionColl.update({name: 'emma'}, {$set: {age: 21}}, {multi: true}));
    assert.commandWorked(sessionColl.update({name: 'liam'}, {$set: {age: 56}}, {multi: true}));
    assert.commandWorked(session.commitTransaction_forTesting());
    session.endSession();

    // The shard hosts both orphaned (liam) and non-orphaned (emma) documents. Consequently, only
    // one update event is notified.
    // TODO (SERVER-65859): The second update event will be filtered out when the ticket is
    // completed.
    assert.soon(() => changeStream.hasNext(), 'A first update event is expected');
    assert.eq(changeStream.next().operationType, 'update');
    assert.soon(() => changeStream.hasNext(), 'A second update event is expected');
    assert.eq(changeStream.next().operationType, 'update');
    assert(!changeStream.hasNext());

    // Both orphaned (liam) and non-orphaned (emma) documents on the shard have been updated.
    assert.eq(21, st.shard0.getCollection(collNS).findOne({_id: -2}).age);
    assert.eq(56, st.shard0.getCollection(collNS).findOne({_id: 5}).age);
}

jsTest.log('Direct deletes (via a transaction to a shard) of both orphaned and non-orphaned' +
           'documents generate events only for operations on non-orphaned documents');
{
    // Send a direct transaction to a shard deleting both orphaned and non-orphaned documents.
    const session = st.rs0.getPrimary().getDB(dbName).getMongo().startSession();
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);
    session.startTransaction();
    assert.commandWorked(sessionColl.remove({name: 'emma'}));
    assert.commandWorked(sessionColl.remove({name: 'liam'}));
    assert.commandWorked(session.commitTransaction_forTesting());
    session.endSession();

    // The shard hosts both orphaned (liam) and non-orphaned (emma) documents. Consequently, only
    // one update event is notified.
    // TODO (SERVER-65859): The second delete event will be filtered out when the ticket is
    // completed.
    assert.soon(() => changeStream.hasNext(), 'A first delete event is expected');
    assert.eq(changeStream.next().operationType, 'delete');
    assert.soon(() => changeStream.hasNext(), 'A second delete event is expected');
    assert.eq(changeStream.next().operationType, 'delete');
    assert(!changeStream.hasNext());

    // Both orphaned (liam) and non-orphaned (emma) documents on the shard have been removed.
    assert.eq(null, st.shard0.getCollection(collNS).findOne({_id: -2}));
    assert.eq(null, st.shard0.getCollection(collNS).findOne({_id: 5}));
}

////////////////////////////////////////////////////////////////////////////////////////////////////

jsTest.log('The collection drop generates a drop event');
{
    coll.drop();

    // Essentially, this verifies that the operation before dropping the collection did not notify
    // additional and unexpected events.
    assert.soon(() => changeStream.hasNext(), 'A drop event is expected');
    assert.eq(changeStream.next().operationType, 'drop');
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Batched deletes from router and to shard
////////////////////////////////////////////////////////////////////////////////////////////////////

// Set the database to use batched deletes.
const db2 = st.rs0.getPrimary().getDB(dbName);
assert.commandWorked(db2.adminCommand({setParameter: 1, batchedDeletesTargetStagedDocBytes: 0}));
assert.commandWorked(db2.adminCommand({setParameter: 1, batchedDeletesTargetBatchTimeMS: 0}));
assert.commandWorked(db2.adminCommand({setParameter: 1, batchedDeletesTargetBatchDocs: 2}));

// Create a non-sharded collection.
const coll2 = db2.getCollection(collName);

// Setup a change stream on the collection to receive real-time events on any data changes.
const changeStream2 = coll2.watch([]);

jsTest.log('A batched delete from router generates only one delete event');
{
    // Insert two documents in the collection (see 'batchedDeletesTargetBatchDocs') and skip the
    // generated events.
    assert.commandWorked(coll2.insert({_id: 0, name: 'volkswagen'}));
    assert.commandWorked(coll2.insert({_id: 1, name: 'renault'}));
    assert.soon(() => changeStream2.hasNext(), 'A first insert event is expected');
    assert.eq(changeStream2.next().operationType, 'insert');
    assert.soon(() => changeStream2.hasNext(), 'A second insert event is expected');
    assert.eq(changeStream2.next().operationType, 'insert');
    assert(!changeStream2.hasNext());

    // Delete all documents in batch from the collection.
    assert.commandWorked(coll2.deleteMany({_id: {$gte: 0}}));

    // Actually only one delete operation is performed. Consequently, only one delete event is
    // notified.
    // TODO (SERVER-65859): The second delete event will be filtered out when the ticket is
    // completed.
    assert.soon(() => changeStream2.hasNext(), 'A first delete event is expected');
    assert.eq(changeStream2.next().operationType, 'delete');
    assert.soon(() => changeStream2.hasNext(), 'A second delete event is expected for now');
    assert.eq(changeStream2.next().operationType, 'delete');
    assert(!changeStream2.hasNext());

    // All documents have been removed from the collection.
    assert.eq(0, coll2.find().itcount());
}

jsTest.log('A batched delete to shard generates only one delete event');
{
    // Insert two documents in the collection (see 'batchedDeletesTargetBatchDocs') and skip the
    // generated events.
    assert.commandWorked(coll2.insert({_id: 0, name: 'volkswagen'}));
    assert.commandWorked(coll2.insert({_id: 1, name: 'renault'}));
    assert.soon(() => changeStream2.hasNext(), 'A first insert event is expected');
    assert.eq(changeStream2.next().operationType, 'insert');
    assert.soon(() => changeStream2.hasNext(), 'A second insert event is expected');
    assert.eq(changeStream2.next().operationType, 'insert');
    assert(!changeStream2.hasNext());

    // Delete all documents in batch from the collection.
    assert.commandWorked(st.shard0.getCollection(collNS).deleteMany({_id: {$gte: 0}}));

    // Actually only one delete operation is performed. Consequently, only one delete event is
    // notified.
    // TODO (SERVER-65859): The second delete event will be filtered out when the ticket is
    // completed.
    assert.soon(() => changeStream2.hasNext(), 'A first delete event is expected');
    assert.eq(changeStream2.next().operationType, 'delete');
    assert.soon(() => changeStream2.hasNext(), 'A second delete event is expected for now');
    assert.eq(changeStream2.next().operationType, 'delete');
    assert(!changeStream2.hasNext());

    // All documents have been removed from the collection.
    assert.eq(0, coll2.find().itcount());
}

////////////////////////////////////////////////////////////////////////////////////////////////////

jsTest.log('The collection drop generates a drop event');
{
    coll2.drop();

    // Essentially, this verifies that the operation before dropping the collection did not notify
    // additional and unexpected events.
    assert.soon(() => changeStream2.hasNext(), 'A drop event is expected');
    assert.eq(changeStream2.next().operationType, 'drop');
}

suspendRangeDeletionShard0.off();

st.stop();
})();
