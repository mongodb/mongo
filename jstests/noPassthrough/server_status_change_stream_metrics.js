/**
 * Tests for serverStatus metrics about change streams.
 */
(function() {
"use strict";

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

db.coll.aggregate([{$changeStream: {}}]);
checkChangeStreamMetrics(db, 1, 0);

db.coll.aggregate([{$changeStream: {showExpandedEvents: true}}]);
checkChangeStreamMetrics(db, 2, 1);

db.coll.explain().aggregate([{$changeStream: {}}]);
checkChangeStreamMetrics(db, 3, 1);

db.coll.explain().aggregate([{$changeStream: {showExpandedEvents: true}}]);
checkChangeStreamMetrics(db, 4, 2);

rst.stopSet();
}());
