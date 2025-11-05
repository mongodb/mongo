/**
 * Tests that deprioritized operations are totalized.
 *
 * @tags: [
 *      requires_wiredtiger,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            // Force the query to yield frequently to better expose the low-priority behavior.
            internalQueryExecYieldIterations: 1,
            storageEngineConcurrencyAdjustmentAlgorithm: "fixedConcurrentTransactionsWithPrioritization",
            storageEngineHeuristicDeprioritizationEnabled: true,
            storageEngineHeuristicNumYieldsDeprioritizeThreshold: 3,
        },
    },
});

rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const db = primary.getDB(jsTestName());
const coll = db.coll;

coll.insert([{_id: 1}, {_id: 2}, {_id: 3}]);

const beforeFindStats = primary.adminCommand({serverStatus: 1}).queues.execution;
coll.find().batchSize(1).toArray();
const afterNormalFindStats = primary.adminCommand({serverStatus: 1}).queues.execution;
// Verify normal ops don't affect total.
assert.eq(beforeFindStats.totalDeprioritizations, afterNormalFindStats.totalDeprioritizations);
coll.find().batchSize(3).toArray();
const afterLowFindStats = primary.adminCommand({serverStatus: 1}).queues.execution;
// Verify low priority ops are totalized.
assert.gt(afterLowFindStats.totalDeprioritizations, beforeFindStats.totalDeprioritizations);

rst.stopSet();
