/**
 * Verify that write operations on orphaned documents (1) do not show up unexpected events in change
 * streams and (2) have a certain behavior on the persisted data.
 *
 * The behavior is tested in the following scenarios:
 *   - Test case 1: Direct operations to shard on orphaned documents
 *   - Test case 2: Broadcasted operations (from router) on orphaned documents
 *   - Test case 3: Transaction from router updating both orphaned and owned documents
 *   - Test case 4: Transaction to shard updating both orphaned and owned documents
 */
(function() {
'use strict';

load('jstests/libs/fail_point_util.js');  // For configureFailPoint

// Asserts that there is no event.
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

const dbName = 'test';
const collName = 'foo';
const collNS = dbName + '.' + collName;

// Enable explicitly the periodic no-op writer to allow the router to process change stream events
// coming from all shards. This is enabled for production clusters by default.
const st = new ShardingTest({
    mongos: 1,
    config: 1,
    shards: 2,
    rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
    other: {enableBalancer: false}
});

// Suspend the range deletion on the first shard to force the orphaned documents to stay here after
// the chunks have been moved to the second shard.
let suspendRangeDeletionShard0 = configureFailPoint(st.shard0, 'suspendRangeDeletion');

// Create a shard collection with documents having both a key field and a non-key field.
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: collNS, key: {_id: 1}}));
const mongosColl = st.s.getCollection(collNS);
assert.commandWorked(mongosColl.insert({_id: -2, name: 'emma', age: 20}));    // Test case 4
assert.commandWorked(mongosColl.insert({_id: -1, name: 'olivia', age: 25}));  // Test case 3
assert.commandWorked(mongosColl.insert({_id: 0, name: 'matt', age: 30}));     // Test case 1
assert.commandWorked(mongosColl.insert({_id: 1, name: 'matt', age: 35}));     // Test case 1
assert.commandWorked(mongosColl.insert({_id: 2, name: 'john', age: 40}));     // Test case 2
assert.commandWorked(mongosColl.insert({_id: 3, name: 'robert', age: 45}));   // Test case 2
assert.commandWorked(mongosColl.insert({_id: 4, name: 'robert', age: 50}));   // Test case 2
assert.commandWorked(mongosColl.insert({_id: 5, name: 'james', age: 55}));    // Test case 3
assert.commandWorked(mongosColl.insert({_id: 6, name: 'liam', age: 60}));     // Test case 4

// Move the chunk to the second shard leaving orphaned documents on the first shard.
assert.commandWorked(st.s.adminCommand({split: collNS, middle: {_id: 0}}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: collNS, find: {_id: 0}, to: st.shard1.shardName}));

// Setup a change stream on the collection to receive real-time events on any data changes.
const changeStream = mongosColl.watch([]);

////////////////////////////////////////////////////////////////////////////////////////////////////
// Test case 1: Direct operations to shard on orphaned documents
////////////////////////////////////////////////////////////////////////////////////////////////////

jsTest.log('A direct insert to a shard of an orphaned document does not generate an insert event');
{
    // Direct insert to first shard of an orphaned document.
    assert.commandWorked(st.shard0.getCollection(collNS).insert({_id: 7, name: 'ken', age: 65}));

    // No event is notified.
    assertNoChanges(changeStream);

    // The orphaned document on first shard has been inserted.
    assert.neq(null, st.shard0.getCollection(collNS).findOne({_id: 7}));
}

jsTest.log('A direct update to a shard of an orphaned document does not generate an update event');
{
    // Send a direct update to first shard on an orphaned document.
    assert.commandWorked(st.shard0.getCollection(collNS).update({name: 'ken'}, {$set: {age: 66}}));

    // No change stream event is generated.
    assertNoChanges(changeStream);

    // The orphaned document on first shard has been updated.
    assert.eq(66, st.shard0.getCollection(collNS).findOne({_id: 7}).age);
}

jsTest.log('A direct delete to a shard of an orphaned document does generate an update event');
{
    // Send a direct delete to first shard on an orphaned document.
    assert.commandWorked(st.shard0.getCollection(collNS).remove({name: 'ken'}));

    // No change stream event is generated.
    assertNoChanges(changeStream);

    // The orphaned document on first shard has been removed.
    assert.eq(null, st.shard0.getCollection(collNS).findOne({_id: 7}));
}

jsTest.log('A direct update to a shard of multi-documents does not generate update events');
{
    // Send a direct update to first shard on two orphaned documents.
    assert.commandWorked(
        st.shard0.getCollection(collNS).update({name: 'matt'}, {$set: {age: 31}}, {multi: true}));

    // No change stream event is generated.
    assertNoChanges(changeStream);

    // The orphaned documents on first shard have been updated
    assert.eq(31, st.shard0.getCollection(collNS).findOne({_id: 0}).age);
    assert.eq(31, st.shard0.getCollection(collNS).findOne({_id: 1}).age);
}

jsTest.log('A direct delete to a shard of multi-documents does not generate delete events');
{
    // Send a direct delete to first shard on an orphaned document.
    assert.commandWorked(st.shard0.getCollection(collNS).remove({name: 'matt'}));

    // No change stream event is generated.
    assertNoChanges(changeStream);

    // The orphaned documents on first shard have been removed.
    assert.eq(null, st.shard0.getCollection(collNS).findOne({_id: 0}));
    assert.eq(null, st.shard0.getCollection(collNS).findOne({_id: 1}));
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Test case 2: Broadcasted operations (from router) on orphaned documents
////////////////////////////////////////////////////////////////////////////////////////////////////

jsTest.log('A broadcasted update of a single document generates an update event');
{
    // Send a broadcasted update (query on non-key field) on a single document to all the shards.
    assert.commandWorked(mongosColl.update({name: 'john'}, {$set: {age: 41}}, {multi: true}));

    // The document is hosted by the second shard and the update event is notified. The first shard
    // still hosts the orphaned document so no additional event must be notified.
    assert.soon(() => changeStream.hasNext(), 'An update event is expected');
    assert.eq(changeStream.next().operationType, 'update');
    assertNoChanges(changeStream);

    // The orphaned document on first shard has not been updated, unlike the owned one on the second
    // shard.
    assert.eq(40, st.shard0.getCollection(collNS).findOne({_id: 2}).age);
    assert.eq(41, st.shard1.getCollection(collNS).findOne({_id: 2}).age);
}

jsTest.log('A broadcasted delete of a single document generates a delete event');
{
    // Send a broadcasted delete (query on non-key field) on a single document to all the shards.
    assert.commandWorked(mongosColl.remove({name: 'john'}));

    // The document is hosted by the second shard and the delete event is notified. The first shard
    // still hosts the orphaned document so no additional event must be notified.
    assert.soon(() => changeStream.hasNext(), 'A delete event is expected');
    assert.eq(changeStream.next().operationType, 'delete');
    assertNoChanges(changeStream);

    // The orphaned document on first shard has not been removed, unlike the owned one on the second
    // shard.
    assert.neq(null, st.shard0.getCollection(collNS).findOne({_id: 2}));
    assert.eq(null, st.shard1.getCollection(collNS).findOne({_id: 2}));
}

jsTest.log('A broadcasted update of multi-documents generates more update events');
{
    // Send a broadcasted update (query on non-key field) on two documents to all the shards.
    assert.commandWorked(mongosColl.update({name: 'robert'}, {$set: {age: 46}}, {multi: true}));

    // The documents are hosted by the second shard and two delete events are notified. The first
    // shard still hosts the orphaned documents so no additional event must be notified.
    assert.soon(() => changeStream.hasNext(), 'A first update event is expected');
    assert.eq(changeStream.next().operationType, 'update');
    assert.soon(() => changeStream.hasNext(), 'A second update event is expected');
    assert.eq(changeStream.next().operationType, 'update');
    assertNoChanges(changeStream);

    // The orphaned documents on first shard have not been updated, unlike the owned ones on the
    // second shard.
    assert.eq(45, st.shard0.getCollection(collNS).findOne({_id: 3}).age);
    assert.eq(50, st.shard0.getCollection(collNS).findOne({_id: 4}).age);
    assert.eq(46, st.shard1.getCollection(collNS).findOne({_id: 3}).age);
    assert.eq(46, st.shard1.getCollection(collNS).findOne({_id: 4}).age);
}

jsTest.log('A broadcasted delete of multi-documents generates more delete events');
{
    // Send a broadcasted delete (query on non-key field) on two documents to all the shards.
    assert.commandWorked(mongosColl.remove({name: 'robert'}));

    // The documents are hosted by the second shard and two delete events are notified. The first
    // shard still hosts the orphaned documents so no additional event must be notified.
    assert.soon(() => changeStream.hasNext(), 'A first delete event is expected');
    assert.eq(changeStream.next().operationType, 'delete');
    assert.soon(() => changeStream.hasNext(), 'A second delete event is expected');
    assert.eq(changeStream.next().operationType, 'delete');
    assertNoChanges(changeStream);

    // The orphaned documents on first shard have not been removed, unlike the owned ones on the
    // second shard.
    assert.neq(null, st.shard0.getCollection(collNS).findOne({_id: 3}));
    assert.neq(null, st.shard0.getCollection(collNS).findOne({_id: 4}));
    assert.eq(null, st.shard1.getCollection(collNS).findOne({_id: 3}));
    assert.eq(null, st.shard1.getCollection(collNS).findOne({_id: 4}));
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Test case 3: Transaction from router updating both orphaned and owned documents
////////////////////////////////////////////////////////////////////////////////////////////////////

jsTest.log('Broadcasted updates (via a transaction through the router) of both orphaned and owned' +
           'documents generate events only for operations on owned documents');
{
    // Send a broadcasted transaction to the router updating both orphaned and owned documents.
    const session = st.s.startSession();
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);
    session.startTransaction();
    assert.commandWorked(sessionColl.update({name: 'olivia'}, {$set: {age: 26}}, {multi: true}));
    assert.commandWorked(sessionColl.update({name: 'james'}, {$set: {age: 56}}, {multi: true}));
    assert.commandWorked(session.commitTransaction_forTesting());
    session.endSession();

    // The primary shard hosts orphaned (james) and owned (olivia) documents, whereas the second
    // shard hosts an owned document (james). Consequently, two update events are notified.
    assert.soon(() => changeStream.hasNext(), 'A first update event is expected');
    assert.eq(changeStream.next().operationType, 'update');
    assert.soon(() => changeStream.hasNext(), 'A second update event is expected');
    assert.eq(changeStream.next().operationType, 'update');
    assertNoChanges(changeStream);

    // The orphaned document on first shard (james) has not been updated, unlike the owned ones on
    // both primary and second shards (olivia and james).
    assert.eq(26, st.shard0.getCollection(collNS).findOne({_id: -1}).age);
    assert.eq(55, st.shard0.getCollection(collNS).findOne({_id: 5}).age);
    assert.eq(56, st.shard1.getCollection(collNS).findOne({_id: 5}).age);
}

jsTest.log('Broadcasted deletes (via a transaction through the router) of both orphaned and owned' +
           'documents generate events only for operations on owned documents');
{
    // Send a broadcasted transaction to the router deleting both orphaned and owned documents.
    const session = st.s.startSession();
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);
    session.startTransaction();
    assert.commandWorked(sessionColl.remove({name: 'olivia'}));
    assert.commandWorked(sessionColl.remove({name: 'james'}));
    assert.commandWorked(session.commitTransaction_forTesting());
    session.endSession();

    // The primary shard hosts orphaned (james) and owned (olivia) documents, whereas the second
    // shard hosts an owned document (james). Consequently, two delete events are notified.
    assert.soon(() => changeStream.hasNext(), 'A first delete event is expected');
    assert.eq(changeStream.next().operationType, 'delete');
    assert.soon(() => changeStream.hasNext(), 'A second delete event is expected');
    assert.eq(changeStream.next().operationType, 'delete');
    assertNoChanges(changeStream);

    // The orphaned document on first shard (james) has not been removed, unlike the owned ones on
    // both primary and second shards (olivia and james).
    assert.eq(null, st.shard0.getCollection(collNS).findOne({_id: -1}));
    assert.neq(null, st.shard0.getCollection(collNS).findOne({_id: 5}));
    assert.eq(null, st.shard1.getCollection(collNS).findOne({_id: 5}));
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Test case 4: Transaction to shard updating both orphaned and owned documents
////////////////////////////////////////////////////////////////////////////////////////////////////

jsTest.log('Direct updates (via a transaction to a shard) of both orphaned and owned documents' +
           'generate events only for operations on owned documents');
{
    // Send a direct transaction to a shard updating both orphaned and owned documents.
    const session = st.rs0.getPrimary().getDB(dbName).getMongo().startSession();
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);
    session.startTransaction();
    assert.commandWorked(sessionColl.update({name: 'emma'}, {$set: {age: 21}}, {multi: true}));
    assert.commandWorked(sessionColl.update({name: 'liam'}, {$set: {age: 61}}, {multi: true}));
    assert.commandWorked(session.commitTransaction_forTesting());
    session.endSession();

    // The shard hosts both orphaned (liam) and non-orphaned (emma) documents. Consequently, only
    // one update event is notified.
    assert.soon(() => changeStream.hasNext(), 'A first update event is expected');
    assert.eq(changeStream.next().operationType, 'update');
    assertNoChanges(changeStream);

    // Both orphaned (liam) and owned (emma) documents on the shard have been updated.
    assert.eq(21, st.shard0.getCollection(collNS).findOne({_id: -2}).age);
    assert.eq(61, st.shard0.getCollection(collNS).findOne({_id: 6}).age);
}

jsTest.log('Direct deletes (via a transaction to a shard) of both orphaned and owned documents' +
           'generate events only for operations on owned documents');
{
    // Send a direct transaction to a shard deleting both orphaned and owned documents.
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
    assert.soon(() => changeStream.hasNext(), 'A first delete event is expected');
    assert.eq(changeStream.next().operationType, 'delete');
    assertNoChanges(changeStream);

    // Both orphaned (liam) and owned (emma) documents on the shard have been removed.
    assert.eq(null, st.shard0.getCollection(collNS).findOne({_id: -2}));
    assert.eq(null, st.shard0.getCollection(collNS).findOne({_id: 6}));
}

////////////////////////////////////////////////////////////////////////////////////////////////////

jsTest.log('The collection drop generates a drop event');
{
    mongosColl.drop();

    // Essentially, this verifies that the operation before dropping the collection did not notify
    // additional and unexpected events.
    assert.soon(() => changeStream.hasNext(), 'A drop event is expected');
    assert.eq(changeStream.next().operationType, 'drop');
}

suspendRangeDeletionShard0.off();

st.stop();
})();
