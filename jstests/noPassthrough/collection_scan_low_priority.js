/**
 * Tests that unbounded collections scans access the storage engine with low priority.
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

const runTest = function(options) {
    assert.commandWorked(db.createCollection(coll.getName(), options));
    assert.commandWorked(coll.insert({_id: 0}));
    assert.commandWorked(coll.insert({_id: 1}));

    const numLowPriority = function() {
        return db.serverStatus()
            .wiredTiger.concurrentTransactions.read.lowPriority.finishedProcessing;
    };

    const testScanDeprioritized = function(direction) {
        const numLowPriorityBefore = numLowPriority();
        coll.find().hint({$natural: direction}).itcount();
        assert.gt(numLowPriority(), numLowPriorityBefore);
    };
    testScanDeprioritized(1);
    testScanDeprioritized(-1);

    const testScanSortLimitDeprioritized = function(direction) {
        const numLowPriorityBefore = numLowPriority();
        coll.find().hint({$natural: direction}).sort({_id: 1}).limit(1).itcount();
        assert.gt(numLowPriority(), numLowPriorityBefore);
    };
    testScanSortLimitDeprioritized(1);
    testScanSortLimitDeprioritized(-1);

    const testScanLimitNotDeprioritized = function(direction) {
        const numLowPriorityBefore = numLowPriority();
        coll.find().hint({$natural: direction}).limit(1).itcount();
        assert.eq(numLowPriority(), numLowPriorityBefore);
    };
    testScanLimitNotDeprioritized(1);
    testScanLimitNotDeprioritized(-1);

    assert(coll.drop());
};

runTest({});
runTest({clusteredIndex: {key: {_id: 1}, unique: true}});

MongoRunner.stopMongod(conn);
}());
