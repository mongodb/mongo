/**
 * Checks the statistic scheme of the different execution control algorithms.
 *
 * @tags: [
 *      featureFlagMultipleTicketPoolsExecutionControl,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

function assertTicketStatsScheme(bool, obj) {
    assert.eq(bool, obj.hasOwnProperty("out"));
    assert.eq(bool, obj.hasOwnProperty("available"));
    assert.eq(bool, obj.hasOwnProperty("totalTickets"));
}

function assertNormalScheme(obj) {
    assertTicketStatsScheme(true, obj);
    assert.eq(true, obj.hasOwnProperty("exempt"));
    assertTicketStatsScheme(false, obj.exempt);
    assert.eq(false, obj.hasOwnProperty("lowPriority"));
    assert.eq(true, obj.hasOwnProperty("normalPriority"));
    assertTicketStatsScheme(false, obj.normalPriority);
}

function assertDeprioritizationScheme(obj) {
    assertTicketStatsScheme(true, obj);
    assert.eq(true, obj.hasOwnProperty("lowPriority"));
    assertTicketStatsScheme(true, obj.lowPriority);
    assert.eq(true, obj.hasOwnProperty("normalPriority"));
    assertTicketStatsScheme(true, obj.normalPriority);
    assert.eq(true, obj.hasOwnProperty("exempt"));
    assertTicketStatsScheme(false, obj.exempt);
}

const rst = new ReplSetTest({
    nodes: 1,
});

rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const db = primary.getDB(jsTestName());

assert.commandWorked(
    rst.getPrimary().adminCommand({
        setParameter: 1,
        storageEngineConcurrencyAdjustmentAlgorithm: "throughputProbing",
    }),
);

const serverStatus = db.serverStatus().queues.execution.read;
assertNormalScheme(serverStatus);

assert.commandWorked(
    rst.getPrimary().adminCommand({
        setParameter: 1,
        storageEngineConcurrencyAdjustmentAlgorithm: "fixedConcurrentTransactions",
    }),
);

const serverStatusFixedTxns = db.serverStatus().queues.execution.read;
assertNormalScheme(serverStatusFixedTxns);

assert.commandWorked(
    rst.getPrimary().adminCommand({
        setParameter: 1,
        storageEngineConcurrencyAdjustmentAlgorithm: "fixedConcurrentTransactionsWithPrioritization",
    }),
);

const serverStatusFixedTxnsPrioritization = db.serverStatus().queues.execution.read;
assertDeprioritizationScheme(serverStatusFixedTxnsPrioritization);

rst.stopSet();
