/**
 * Tests "metrics.query.allowDiskUseFalse" counter.
 */

(function() {
"use strict";

const mongod = MongoRunner.runMongod();
const db = mongod.getDB(jsTest.name());

const kAllowDiskUseFalse = "allowDiskUseFalse";

function getQueryMetrics() {
    return db.serverStatus().metrics.query;
}

function getAllowDiskUseFalseCounter() {
    return getQueryMetrics()[kAllowDiskUseFalse];
}

function runTest(cmdObject) {
    let allowDiskUseFalseValue = getAllowDiskUseFalseCounter();

    // Make sure that allowDiskUseFalse counter increments by 1 if a command with
    // {allowDiskUse:false} is called.
    assert.commandWorked(db.runCommand(Object.assign(cmdObject, {allowDiskUse: false})));
    allowDiskUseFalseValue += 1;
    assert.eq(allowDiskUseFalseValue, getAllowDiskUseFalseCounter());

    // Make sure that allowDiskUseFalse counter stays the same if a command with
    // {allowDiskUse:true} is called.
    assert.commandWorked(db.runCommand(Object.assign(cmdObject, {allowDiskUse: true})));
    assert.eq(allowDiskUseFalseValue, getAllowDiskUseFalseCounter());

    // Make sure that allowDiskUseFalse counter stays the same if a command without allowDiskUse
    // parameter is called.
    assert.commandWorked(db.runCommand(cmdObject));
    assert.eq(allowDiskUseFalseValue, getAllowDiskUseFalseCounter());

    // Make sure that allowDiskUseFalse counter increments by 1 if a command with
    // {allowDiskUse:false} is called a second time.
    assert.commandWorked(db.runCommand(Object.assign(cmdObject, {allowDiskUse: false})));
    allowDiskUseFalseValue += 1;
    assert.eq(allowDiskUseFalseValue, getAllowDiskUseFalseCounter());
}

// Check if the metrics.query.allowDiskUseFalse exists and by default equals to 0.
assert(getQueryMetrics().hasOwnProperty(kAllowDiskUseFalse));
assert.eq(0, getAllowDiskUseFalseCounter());

// Test on a collection.
const simpleCollName = "simple_coll";
const simpleColl = db[simpleCollName];
simpleColl.drop();
runTest({aggregate: simpleColl.getName(), pipeline: [], cursor: {}});
runTest({find: simpleColl.getName()});

// Test on a view.
const simpleViewName = "simple_view";
const simpleView = db[simpleViewName];
simpleView.drop();
assert.commandWorked(db.createView(simpleViewName, simpleCollName, [{$match: {a: 1}}]));
runTest({aggregate: simpleView.getName(), pipeline: [], cursor: {}});
runTest({find: simpleView.getName()});

MongoRunner.stopMongod(mongod);
})();
