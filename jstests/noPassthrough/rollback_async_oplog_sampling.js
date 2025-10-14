/**
 * Tests that rollback interrupts the oplog cap maintainer thread, and that it restarts after
 * rollback is complete.
 *
 * @tags: [
 *      requires_persistence,
 *      requires_wiredtiger,
 * ]
 */
import {kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";
import {RollbackTest} from "jstests/replsets/libs/rollback_test.js";

const rst = new ReplSetTest({
    name: jsTestName(),
    nodes: [{}, {}, {rsConfig: {priority: 0}}],
    useBridge: true,
    nodeOptions: {
        setParameter: {
            "oplogSamplingAsyncEnabled": true,
            "collectionSamplingLogIntervalSeconds": 1,
            "failpoint.hangBeforeOplogSampling": tojson({mode: "alwaysOn"}),
        },
    },
    settings: {chainingAllowed: false},
});
rst.startSet();
rst.initiateWithHighElectionTimeout();

const rollbackTest = new RollbackTest(jsTestName(), rst);

const primary = rollbackTest.getPrimary();
const testDb = primary.getDB("test");
const coll = testDb.getCollection(jsTestName());

// Wait for the oplog cap maintainer thread to start and hang.
assert.commandWorked(
    primary.adminCommand({
        waitForFailPoint: "hangBeforeOplogSampling",
        timesEntered: 1,
        maxTimeMS: kDefaultWaitForFailPointTimeout,
    }),
);

// Populate the nodes with some data.
const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < 1000; i++) {
    bulk.insert({x: i});
}
assert.commandWorked(bulk.execute());

rollbackTest.awaitLastOpCommitted();
rollbackTest.transitionToRollbackOperations();

let interruptCount = assert.commandWorked(primary.getDB("admin").runCommand({serverStatus: 1}))
                         .oplogTruncationThread.interruptCount;
assert(interruptCount == 0);

assert.commandWorked(coll.insert({x: 1000}));

rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();

// OplogCapMaintainerThread interrupted.
checkLog.containsJson(primary, 11212201);

// OplogCapMaintainerThread shutdown.
checkLog.containsJson(primary, 7474901);

interruptCount = assert.commandWorked(primary.getDB("admin").runCommand({serverStatus: 1}))
                     .oplogTruncationThread.interruptCount;
assert(interruptCount == 1);

// OplogCapMaintainerThread never finished scanning up to this point.
assert(checkLog.checkContainsWithCountJson(primary, 22382, {}, /*expectedCount=*/ 0));

// The OplogCapMaintainerThread gets restarted at the end of FCBIS, so turn off the fail point.
assert.commandWorked(primary.getDB("admin").adminCommand(
    {configureFailPoint: "hangBeforeOplogSampling", mode: "off"}));

// OplogCapMaintainerThread finishes scanning the oplog exactly once.
assert.soon(() => {
    return checkLog.checkContainsWithCountJson(primary, 22382, {}, /*expectedCount=*/ 1);
});

rollbackTest.transitionToSteadyStateOperations();
rollbackTest.stop();

// Make sure shutdown interrupted the OplogCapMaintainerThread.
const subStr = "11212204.*OplogCapMaintainerThread interrupted.*at shutdown";
assert(rawMongoProgramOutput(subStr).search(subStr) !== -1);
