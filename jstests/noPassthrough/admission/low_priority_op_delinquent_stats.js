/**
 * Checks that an operation that is deprioritized reports the delinquency statistic as part of the
 * lowPriority object.
 *
 * # TODO (SERVER-112729): integrate this test with delinquency_ops.js.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// The failpoint will wait for this long before yielding for every iteration.
const waitPerIterationMs = 200;
// This is how long we consider an operation as delinquent.
const delinquentIntervalMs = waitPerIterationMs - 20;
const findComment = "delinquent_ops.js-COMMENT";

// We start the server with a high threshold for delinquency. This is to avoid any internal
// operations which are considered delinquent from polluting the delinquency counters.  This test
// only focuses on the mechanism by which delinquency is determined and reported, and does not aim
// to enforce that all (or some fraction) of operations are not delinquent. Also lower the threshold
// of parameters to generate low priority operations.
let lowPriorityParams = {
    executionControlHeuristicDeprioritizationEnabled: true,
    internalQueryExecYieldIterations: 1,
    executionControlConcurrencyAdjustmentAlgorithm: "fixedConcurrentTransactionsWithPrioritization",
    featureFlagRecordDelinquentMetrics: true,
    delinquentAcquisitionIntervalMillis: delinquentIntervalMs,
    internalQueryStatsRateLimit: -1,
    overdueInterruptCheckIntervalMillis: delinquentIntervalMs * 100,
    overdueInterruptCheckSamplingRate: 1.0, // For this test we sample 100% of the time.
};

const rst = new ReplSetTest({nodes: 1, nodeOptions: {setParameter: lowPriorityParams}});

rst.startSet();
rst.initiate();
const primary = rst.getPrimary();
const dbName = jsTestName();
const db = primary.getDB(dbName);
const coll = db.coll;

assert.commandWorked(db.createCollection(coll.getName()));
assert.commandWorked(coll.insert([{_id: 0}, {_id: 1}, {_id: 2}]));

const beforeFindStats = primary.adminCommand({serverStatus: 1}).queues.execution.read.lowPriority;

const failPoint = configureFailPoint(primary, "setPreYieldWait", {
    waitForMillis: waitPerIterationMs,
    comment: findComment,
});

assert.eq(3, coll.find().batchSize(3).comment(findComment).hint({$natural: 1}).toArray().length);

failPoint.off();

const afterFindStats = primary.adminCommand({serverStatus: 1}).queues.execution.read.lowPriority;

assert.gt(afterFindStats.totalDelinquentAcquisitions, beforeFindStats.totalDelinquentAcquisitions);

rst.stopSet();
