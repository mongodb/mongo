/**
 * Tests "metrics.commands.aggregate" counters.
 */

(function() {
"use strict";

const mongod = MongoRunner.runMongod();
const db = mongod.getDB(jsTest.name());

const kAllowDiskUseTrue = "allowDiskUseTrue";

function getAggregateMetrics() {
    return db.serverStatus().metrics.commands.aggregate;
}

function getAllowDiskUseAggregateCounter() {
    return getAggregateMetrics()[kAllowDiskUseTrue];
}

function testAllowDistUseTrueCounter(coll) {
    let allowDiskUseTrueValue = getAllowDiskUseAggregateCounter();

    // Make sure that allowDiskUseTrue counter increments by 1 is an aggregate command with
    // {allowDiskUse: true} is called.
    assert.eq(0, coll.aggregate([], {allowDiskUse: true}).itcount());
    allowDiskUseTrueValue += 1;
    assert.eq(allowDiskUseTrueValue, getAllowDiskUseAggregateCounter());

    // Make sure that allowDiskUseTrue counter stays the same if aggregate command with
    // {allowDiskUse: false} is called.
    assert.eq(0, coll.aggregate([], {allowDiskUse: false}).itcount());
    assert.eq(allowDiskUseTrueValue, getAllowDiskUseAggregateCounter());

    // Make sure that allowDiskUseTrue counter stays the same if aggregate command without
    // allowDiskUse parameter is called.
    coll.aggregate([]).itcount();
    assert.eq(allowDiskUseTrueValue, getAllowDiskUseAggregateCounter());

    // Make sure that allowDiskUseTrue counter increments by 1 is an aggregate command with
    // {allowDiskUse: true} is called a second time.
    assert.eq(0, coll.aggregate([], {allowDiskUse: true}).itcount());
    allowDiskUseTrueValue += 1;
    assert.eq(allowDiskUseTrueValue, getAllowDiskUseAggregateCounter());
}

// Execute aggregate command to ensure that the metrics.
assert.eq(0, db.coll.aggregate([]).itcount());

// Check if the metrics.commands.aggregate.allowDiskUse exists and by default equals to 0.
assert(getAggregateMetrics().hasOwnProperty(kAllowDiskUseTrue));
assert.eq(0, getAllowDiskUseAggregateCounter());

// Test on a collection.
const simpleTableName = "aggregateMetricsTest";
const simpleColl = db[simpleTableName];
simpleColl.drop();
testAllowDistUseTrueCounter(simpleColl);

// Test on a view.
const simpleViewName = "viewOnAggregateMetricsTest";
const simpleView = db[simpleViewName];
simpleView.drop();
assert.commandWorked(db.createView(simpleViewName, simpleTableName, [{$match: {a: 1}}]));
testAllowDistUseTrueCounter(simpleView);

MongoRunner.stopMongod(mongod);
})();
