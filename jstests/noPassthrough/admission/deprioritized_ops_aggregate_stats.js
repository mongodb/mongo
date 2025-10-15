/**
 * Checks that if an operation is de-prioritized all the statistics are aggregated.
 * @tags: [
 *   featureFlagMultipleTicketPoolsExecutionControl
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

const beforeFindStats = primary.adminCommand({serverStatus: 1}).queues.execution.read;
coll.find().batchSize(1).toArray();
const afterNormalFindStats = primary.adminCommand({serverStatus: 1}).queues.execution.read;
// Verify normal priority stats changes.
assert.gte(afterNormalFindStats.out, beforeFindStats.normalPriority.out);
assert.gte(afterNormalFindStats.available, beforeFindStats.normalPriority.available);
assert.gte(afterNormalFindStats.totalTickets, beforeFindStats.normalPriority.totalTickets);
coll.find().batchSize(3).toArray();
const afterLowFindStats = primary.adminCommand({serverStatus: 1}).queues.execution.read;
// Verify low priority stats changes.
assert.gte(afterLowFindStats.out, beforeFindStats.normalPriority.out);
assert.gte(afterLowFindStats.available, beforeFindStats.normalPriority.available);
assert.gte(afterLowFindStats.totalTickets, beforeFindStats.normalPriority.totalTickets);

// Check the stats aggregate.
assert.gte(afterLowFindStats.out, afterLowFindStats.normalPriority.out + afterLowFindStats.lowPriority.out);
assert.gte(
    afterLowFindStats.available,
    afterLowFindStats.normalPriority.available + afterLowFindStats.lowPriority.available,
);
assert.gte(
    afterLowFindStats.totalTickets,
    afterLowFindStats.normalPriority.totalTickets + afterLowFindStats.lowPriority.totalTickets,
);

rst.stopSet();
