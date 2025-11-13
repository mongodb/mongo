/**
 * Checks the aggregate of tickets statistic happens if the prioritization is enabled.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

function verifyTicketAggregationStats(assertFunc, obj1, obj2) {
    // Step 1: ensure the statistic schema is correct.
    assert.eq(true, obj1.hasOwnProperty("available"));
    assert.eq(true, obj1.hasOwnProperty("out"));
    assert.eq(true, obj1.hasOwnProperty("totalTickets"));
    assert.eq(true, obj2.hasOwnProperty("available"));
    assert.eq(true, obj2.hasOwnProperty("out"));
    assert.eq(true, obj2.hasOwnProperty("totalTickets"));
    // Step 2: compare the statistics. The caller tries to assert compFunc(obj1, obj2).
    // Not comparing out because it depends on whether there is an op running or not.
    assertFunc(obj1.available, obj2.available);
    assertFunc(obj1.totalTickets, obj2.totalTickets);
}

const kNumReadTickets = 5;
const kNumWriteTickets = 5;
const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            executionControlConcurrentReadTransactions: kNumReadTickets,
            executionControlConcurrentReadLowPriorityTransactions: kNumReadTickets,
            executionControlConcurrentWriteTransactions: kNumWriteTickets,
            executionControlConcurrentWriteLowPriorityTransactions: kNumWriteTickets,
        },
    },
});

rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const db = primary.getDB(jsTestName());

assert.commandWorked(
    rst.getPrimary().adminCommand({
        setParameter: 1,
        executionControlConcurrencyAdjustmentAlgorithm: "throughputProbing",
    }),
);

jsTestLog("Verify that there is no totalization of tickets in throughputProbing");
const executionStats = db.serverStatus().queues.execution;
assert.eq(executionStats.prioritizationEnabled, false);
verifyTicketAggregationStats(assert.eq, executionStats.read, executionStats.read.normalPriority);
verifyTicketAggregationStats(assert.eq, executionStats.write, executionStats.write.normalPriority);

jsTestLog("Verify that there is totalization of tickets in fixedConcurrentTransactionsWithPrioritization");
assert.commandWorked(
    rst.getPrimary().adminCommand({
        setParameter: 1,
        executionControlConcurrencyAdjustmentAlgorithm: "fixedConcurrentTransactionsWithPrioritization",
    }),
);
const executionStatsWithPrioritization = db.serverStatus().queues.execution;
assert.eq(executionStatsWithPrioritization.prioritizationEnabled, true);
verifyTicketAggregationStats(
    assert.gt,
    executionStatsWithPrioritization.read,
    executionStatsWithPrioritization.read.normalPriority,
);
verifyTicketAggregationStats(
    assert.gt,
    executionStatsWithPrioritization.write,
    executionStatsWithPrioritization.write.normalPriority,
);

jsTestLog("Verify that there is no totalization of tickets in fixedConcurrentTransactions");
assert.commandWorked(
    rst.getPrimary().adminCommand({
        setParameter: 1,
        executionControlConcurrencyAdjustmentAlgorithm: "fixedConcurrentTransactions",
    }),
);
const executionStatsFixedConcurrency = db.serverStatus().queues.execution;
assert.eq(executionStatsFixedConcurrency.prioritizationEnabled, false);
verifyTicketAggregationStats(
    assert.eq,
    executionStatsFixedConcurrency.read,
    executionStatsFixedConcurrency.read.normalPriority,
);
verifyTicketAggregationStats(
    assert.eq,
    executionStatsFixedConcurrency.write,
    executionStatsFixedConcurrency.write.normalPriority,
);

// TODO SERVER-113367: Once we can enable the heuristic with throughput probing, we need to add cases of the heuristic and algorithms combinations.
rst.stopSet();
