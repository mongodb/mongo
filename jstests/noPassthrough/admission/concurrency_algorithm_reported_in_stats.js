/**
 * Checks the concurrency algorithm is reported on serverStatus.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

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
        executionControlConcurrencyAdjustmentAlgorithm: "throughputProbing",
    }),
);

let algorithm = db.serverStatus().queues.execution.executionControlConcurrencyAdjustmentAlgorithm;
assert.eq(2, algorithm);

assert.commandWorked(
    rst.getPrimary().adminCommand({
        setParameter: 1,
        executionControlConcurrencyAdjustmentAlgorithm: "fixedConcurrentTransactionsWithPrioritization",
    }),
);

algorithm = db.serverStatus().queues.execution.executionControlConcurrencyAdjustmentAlgorithm;
assert.eq(1, algorithm);

assert.commandWorked(
    rst.getPrimary().adminCommand({
        setParameter: 1,
        executionControlConcurrencyAdjustmentAlgorithm: "fixedConcurrentTransactions",
    }),
);

algorithm = db.serverStatus().queues.execution.executionControlConcurrencyAdjustmentAlgorithm;
assert.eq(0, algorithm);

rst.stopSet();
