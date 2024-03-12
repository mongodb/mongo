/**
 * Tests that unbounded index scans access the storage engine with low priority.
 *
 * @tags: [
 *   featureFlagDeprioritizeLowPriorityOperations,
 *   requires_fcv80,
 *   requires_wiredtiger,
 * ]
 */
import {isLinux} from "jstests/libs/os_helpers.js";

if (!isLinux()) {
    quit();
}

const conn = MongoRunner.runMongod();

const db = conn.getDB(jsTestName());
const coll = db.coll;

assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}));

const runTest = function(deprioritize) {
    assert.commandWorked(coll.insert({a: 0}));
    assert.commandWorked(coll.insert({a: 1}));
    assert.commandWorked(coll.createIndexes([{a: 1}, {a: -1}]));

    const numLowPriority = function() {
        return db.serverStatus().admission.execution.read.lowPriority.finishedProcessing;
    };

    const testCoveredScanDeprioritized = function(direction) {
        const numLowPriorityBefore = numLowPriority();
        coll.find().hint({a: direction}).itcount();
        if (deprioritize) {
            assert.gt(numLowPriority(), numLowPriorityBefore);
        } else {
            assert.eq(numLowPriority(), numLowPriorityBefore);
        }
    };
    testCoveredScanDeprioritized(1);
    testCoveredScanDeprioritized(-1);

    const testNonCoveredScanDeprioritized = function(direction) {
        const numLowPriorityBefore = numLowPriority();
        coll.find({b: 1}).hint({a: direction}).itcount();
        if (deprioritize) {
            assert.gt(numLowPriority(), numLowPriorityBefore);
        } else {
            assert.eq(numLowPriority(), numLowPriorityBefore);
        }
    };
    testNonCoveredScanDeprioritized(1);
    testNonCoveredScanDeprioritized(-1);

    const testScanSortLimitDeprioritized = function(direction) {
        const numLowPriorityBefore = numLowPriority();
        coll.find().hint({a: direction}).sort({a: 1}).limit(1).itcount();
        if (deprioritize) {
            assert.gt(numLowPriority(), numLowPriorityBefore);
        } else {
            assert.eq(numLowPriority(), numLowPriorityBefore);
        }
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
};

runTest(true);

assert.commandWorked(
    db.adminCommand({setParameter: 1, deprioritizeUnboundedUserIndexScans: false}));
runTest(false);

MongoRunner.stopMongod(conn);
