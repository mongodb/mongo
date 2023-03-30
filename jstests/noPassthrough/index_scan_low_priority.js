/**
 * Tests that unbounded index scans access the storage engine with low priority.
 *
 * @tags: [
 *   requires_wiredtiger,
 * ]
 */
(function() {
'use strict';

load('jstests/libs/os_helpers.js');

if (!isLinux()) {
    return;
}

const conn = MongoRunner.runMongod();

const db = conn.getDB(jsTestName());
const coll = db.coll;

assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}));

assert.commandWorked(coll.insert({a: 0}));
assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(coll.createIndexes([{a: 1}, {a: -1}]));

const numLowPriority = function() {
    return db.serverStatus().wiredTiger.concurrentTransactions.read.lowPriority.finishedProcessing;
};

const testCoveredScanDeprioritized = function(direction) {
    const numLowPriorityBefore = numLowPriority();
    coll.find().hint({a: direction}).itcount();
    assert.gt(numLowPriority(), numLowPriorityBefore);
};
testCoveredScanDeprioritized(1);
testCoveredScanDeprioritized(-1);

const testNonCoveredScanDeprioritized = function(direction) {
    const numLowPriorityBefore = numLowPriority();
    coll.find({b: 1}).hint({a: direction}).itcount();
    assert.gt(numLowPriority(), numLowPriorityBefore);
};
testNonCoveredScanDeprioritized(1);
testNonCoveredScanDeprioritized(-1);

const testScanSortLimitDeprioritized = function(direction) {
    const numLowPriorityBefore = numLowPriority();
    coll.find().hint({a: direction}).sort({a: 1}).limit(1).itcount();
    assert.gt(numLowPriority(), numLowPriorityBefore);
};
testScanSortLimitDeprioritized(1);
testScanSortLimitDeprioritized(-1);

const testScanLimitNotDeprioritized = function(direction) {
    const numLowPriorityBefore = numLowPriority();
    coll.find().hint({a: direction}).limit(1).itcount();
    assert.eq(numLowPriority(), numLowPriorityBefore);
};
testScanLimitNotDeprioritized(1);
testScanLimitNotDeprioritized(-1);

MongoRunner.stopMongod(conn);
}());
