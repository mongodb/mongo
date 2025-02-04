/**
 * Tests for serverStatus metrics about change streams.
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

function getChangeStreamMetrics(db) {
    const metrics = db.serverStatus().metrics;
    return {
        total: metrics.aggStageCounters["$changeStream"],
        withExpandedEvents: metrics.changeStreams.showExpandedEvents,
    };
}

function checkChangeStreamMetrics(db, expectedTotal, expectedWithExpandedEvents) {
    const metrics = getChangeStreamMetrics(db);
    assert.eq(expectedTotal, metrics.total);
    assert.eq(expectedWithExpandedEvents, metrics.withExpandedEvents);
}

const rst = new ReplSetTest({name: jsTest.name(), nodes: 1});
rst.startSet();
rst.initiate();
const db = rst.getPrimary().getDB(jsTest.name());
const coll = db.getCollection(jsTest.name());

checkChangeStreamMetrics(db, 0, 0);

coll.aggregate([{$changeStream: {}}]);
checkChangeStreamMetrics(db, 1, 0);

coll.aggregate([{$changeStream: {showExpandedEvents: true}}]);
checkChangeStreamMetrics(db, 2, 1);

coll.explain().aggregate([{$changeStream: {}}]);
checkChangeStreamMetrics(db, 3, 1);

coll.explain().aggregate([{$changeStream: {showExpandedEvents: true}}]);
checkChangeStreamMetrics(db, 4, 2);

function checkOplogMetrics(
    db, changeStream, previousMetrics, expectedDocsReturned, expectedDocsScanned) {
    assert.soon(() => changeStream.hasNext());
    // Consume the events.
    while (changeStream.hasNext()) {
        changeStream.next();
    }

    const newMetrics = db.serverStatus().metrics;
    const oplogDocsReturned =
        newMetrics.oplogStats.document.returned - previousMetrics.oplogStats.document.returned;
    const oplogDocsScanned = newMetrics.oplogStats.queryExecutor.scannedObjects -
        previousMetrics.oplogStats.queryExecutor.scannedObjects;
    assert.gte(newMetrics.document.returned, newMetrics.oplogStats.document.returned);
    assert.gte(newMetrics.queryExecutor.scannedObjects,
               newMetrics.oplogStats.queryExecutor.scannedObjects);
    assert.eq(expectedDocsReturned, oplogDocsReturned);
    assert.eq(expectedDocsScanned, oplogDocsScanned);
    // Check oplog stats are added to total stats.
    assert.eq(oplogDocsReturned, newMetrics.document.returned - previousMetrics.document.returned);
    assert.eq(
        oplogDocsScanned,
        newMetrics.queryExecutor.scannedObjects - previousMetrics.queryExecutor.scannedObjects);

    changeStream.close();
}

const changeStream = coll.watch();
const filteredChangeStream = coll.watch([{$match: {operationType: "insert"}}]);
checkChangeStreamMetrics(db, 6, 2);

// These commands will result in 6 oplog events.
assert.commandWorked(coll.insert({_id: 1, string: "Value"}));
assert.commandWorked(coll.update({_id: 1}, {$set: {foo: "bar"}}));
assert.commandWorked(coll.remove({_id: 1}));
assert.commandWorked(coll.insert({_id: 2, string: "vAlue"}));
assert.commandWorked(coll.update({_id: 2}, {$set: {foo: "bar"}}));
assert.commandWorked(coll.remove({_id: 2}));

let previousMetrics = db.serverStatus().metrics;
// Save the resume token of the first 'insert' event.
assert.soon(() => changeStream.hasNext());
const event1 = changeStream.next();
assert.eq(event1.operationType, "insert", event1);
assert.eq(event1.documentKey._id, 1, event1);

// Check all 6 events are scanned and returned.
checkOplogMetrics(db, changeStream, previousMetrics, 6, 7);

// Resume the change stream after the first 'insert'. Only 5 events are scanned and returned.
previousMetrics = db.serverStatus().metrics;
const resumedChangeStream = coll.watch([], {resumeAfter: event1._id});
checkChangeStreamMetrics(db, 7, 2);
checkOplogMetrics(db, resumedChangeStream, previousMetrics, 5, 6);

// New change stream matching only 'insert' operations. 2 events are returned, but all 6 events are
// scanned.
previousMetrics = db.serverStatus().metrics;
checkOplogMetrics(db, filteredChangeStream, previousMetrics, 2, 7);

rst.stopSet();
