/**
 * Test that change streams returns DDL operation on views.
 *
 * @tags: [ requires_fcv_60, ]
 */
(function() {
"use strict";

load('jstests/libs/change_stream_util.js');
load('jstests/libs/collection_drop_recreate.js');

const testDB = db.getSiblingDB(jsTestName());

// Drop all the namespaces accessed in the test.
assertDropCollection(testDB, "base");
assertDropCollection(testDB, "view");
assertDropCollection(testDB, "viewOnView");

// Insert some data on the base collection so that the passthrough suites would finishing setting up
// the collections, and does not generate unexpected operations during the test.
assert.commandWorked(testDB.createCollection("base"));
assert.commandWorked(testDB["base"].insert({_id: 1}));

const dbName = testDB.getName();
const viewPipeline = [{$match: {a: 2}}, {$project: {a: 1}}];

if (!isChangeStreamsVisibilityEnabled(testDB)) {
    const cursor =
        db.getSiblingDB("admin").aggregate([{$changeStream: {allChangesForCluster: true}}]);
    assert.commandWorked(testDB.createView("view", "base", viewPipeline));

    assert(!cursor.hasNext(), () => tojson(cursor.next()));
    return;
}

(function runViewEventAndResumeTest() {
    let cursor = testDB.aggregate([{$changeStream: {showExpandedEvents: true}}]);

    assert.commandWorked(testDB.createView("view", "base", viewPipeline));
    assert.soon(() => cursor.hasNext());

    let event = cursor.next();

    assert(event.clusterTime, event);
    assert(event.wallTime, event);
    assertChangeStreamEventEq(event, {
        operationType: "create",
        ns: {db: dbName, coll: "view"},
        operationDescription: {viewOn: "base", pipeline: viewPipeline}
    });

    // Ensure that we can resume the change stream using a resuming token from the create view
    // event.
    cursor =
        testDB.aggregate([{$changeStream: {resumeAfter: event._id, showExpandedEvents: true}}]);
    assert.commandWorked(testDB.createView("viewOnView", "view", viewPipeline));
    assert.soon(() => cursor.hasNext());

    event = cursor.next();
    assertChangeStreamEventEq(event, {
        operationType: "create",
        ns: {db: dbName, coll: "viewOnView"},
        operationDescription: {viewOn: "view", pipeline: viewPipeline}
    });

    // TODO SERVER-61886 : Add tests for modify a view events.

    assertDropCollection(testDB, "view");

    // TODO SERVER-63306 : This should produce a 'drop' event.
    assert.soon(() => cursor.hasNext());
    const eventForDrop = cursor.next();
    assert(eventForDrop.operationType == "invalidate");
})();

const cst = new ChangeStreamTest(testDB);

// Cannot start a change stream on a view namespace.
assert.commandWorked(testDB.createView("view", "base", viewPipeline));
assert.commandFailedWithCode(
    assert.throws(() => cst.startWatchingChanges({
                     pipeline: [{$changeStream: {showExpandedEvents: true}}],
                     collection: "view",
                     doNotModifyInPassthroughs: true
                 })),
                 ErrorCodes.CommandNotSupportedOnView);

// Creating a collection level change stream before creating a view with the same name, does not
// produce any view related events.
assertDropCollection(testDB, "view");

let cursor = cst.startWatchingChanges({
    pipeline: [{$changeStream: {showExpandedEvents: true}}],
    collection: "view",
    doNotModifyInPassthroughs: true
});

// Create a view, then drop it and create a normal collection with the same name.
assert.commandWorked(testDB.createView("view", "base", viewPipeline));
assertDropCollection(testDB, "view");
assert.commandWorked(testDB.createCollection("view"));

// Confirm that the stream only sees the normal collection creation, not the view events.
let event = cst.getNextChanges(cursor, 1)[0];
assert(event.collectionUUID, event);
assertChangeStreamEventEq(event, {
    operationType: "create",
    ns: {db: dbName, coll: "view"},
    operationDescription: {idIndex: {v: 2, key: {_id: 1}, name: "_id_"}}
});

// Change stream on a single collection does not produce view events.
assertDropCollection(testDB, "view");
cursor = cst.startWatchingChanges({
    pipeline: [{$changeStream: {showExpandedEvents: true}}],
    collection: "base",
    doNotModifyInPassthroughs: true
});

// Verify that the view related operations are ignored, and only the event for insert on the base
// collection is returned.
assert.commandWorked(testDB.createView("view", "base", viewPipeline));
assertDropCollection(testDB, "view");
assert.commandWorked(testDB["base"].insert({_id: 0}));
event = cst.getNextChanges(cursor, 1)[0];
assert(event.collectionUUID, event);
assertChangeStreamEventEq(event, {
    operationType: "insert",
    ns: {db: dbName, coll: "base"},
    fullDocument: {_id: 0},
    documentKey: {_id: 0}
});
}());
