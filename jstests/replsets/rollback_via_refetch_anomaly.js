/**
 * Rolled-back documents must not be visible when querying a recovered secondary.
 *
 * This test attempts to reproduce SERVER-48518. In the following description, 'T' is the tiebreaker
 * node, 'P1' and 'P2' are primaries in terms 1 and 2.
 *
 * - Begin in RollbackTest.kSteadyStateOps with primary P1, all nodes connected:
 *
 *          T
 *        /   \
 *       P1 -  P2
 *  primary   secondary
 *
 * - Insert _id 0 into P1 at timestamp 1.
 * - Transition to kRollbackOps by disconnecting P1 from P2:
 *
 *          T
 *        /
 *       P1    P2
 *  primary   secondary
 *
 * - Insert _id 1 into P1 at timestamp 2.
 *
 *      TS 1            TS 2
 *      insert 0    insert 1
 *  P1  --------------->
 *  P2  --->
 *
 * - Isolate P1 from T, connect P2 to T:
 *
 *          T
 *            \
 *       P1    P2
 *  primary  new primary
 *
 * (Same as RollbackTest.transitionToSyncSourceOperationsBeforeRollback(), except do *not* trigger a
 * stepdown on P1.)
 *
 * - Step up P2, which writes a no-op to its oplog at timestamp 3.
 *
 *      TS 1            TS 2
 *      insert 0    insert 1
 *
 *  P1  --------------->
 *  P2  ----*
 *           \
 *            *-------------------------->
 *                                     no-op
 *                                     TS 3
 *
 * - Delete _id 0 and 1 from P1 at timestamp 4.
 *
 *      TS 1            TS 2                        TS 4
 *      insert 0    insert 1                        delete 0 and 1
 *
 *  P1  --------------------------------------------------------------->
 *  P2  ----*
 *           \
 *            *-------------------------->
 *                                     no-op
 *                                     TS 3
 *
 * - Reconnect P1 to P2 so it rolls back.
 *
 *           T
 *             \
 *        P1 -  P2
 *  rollback  new primary
 *
 * Rollback via refetch undoes the delete of _id 0 by reinserting _id 0 in P1 with an
 * untimestamped write. (It can't undo the delete of _id 1, since P2 doesn't have _id 1.)
 *
 * Before we fixed SERVER-48518, P1 served queries at lastApplied = top of P2's oplog = TS 3,
 * which includes _id 0, _id 1, and _id 0 again (it was reinserted with an untimestamped write).
 * To fix SERVER-48518, P1 won't transition from ROLLBACK until its lastApplied >= max(P1's oplog
 * top, P2's oplog top) = TS 4.
 *
 * - Write to P2 so it advances >= timestamp 4 and satisfies P1's conditions to finish rollback.
 * - Wait for P1 to finish rollback and transition to SECONDARY.
 * - Query P1 and check that rolled-back records aren't visible.
 *
 * To end the test, RollbackTest.transitionToSteadyStateOperations won't work, we've diverged from
 * the state it expects, so we end the test manually. Reconnect P1 to T, enable replication on T,
 * and stop the replica set.
 *
 *           T
 *         /   \
 *        P1 -  P2
 *  secondary  new primary
 *
 * @tags: [
 *   requires_wiredtiger
 * ]
 */

(function() {
"use strict";

load("jstests/core/txns/libs/prepare_helpers.js");
load("jstests/libs/write_concern_util.js");
load("jstests/replsets/libs/rollback_test.js");

const rst = new ReplSetTest({
    nodes: 3,
    useBridge: true,
    nodeOptions: {
        enableMajorityReadConcern: "false",
        setParameter: {logComponentVerbosity: tojsononeline({replication: 2})}
    }
});

rst.startSet();
const config = rst.getReplSetConfig();
config.members[2].priority = 0;
config.settings = {
    chainingAllowed: false
};
rst.initiateWithHighElectionTimeout(config);

const rollbackTest = new RollbackTest(jsTestName(), rst);
const P1 = rollbackTest.getPrimary();
const P2 = rollbackTest.getSecondary();
const T = rollbackTest.getTieBreaker();

jsTestLog(`Node P1: ${P1.host}, P2: ${P2.host}, T: ${T.host}`);

let testDB = P1.getDB(jsTestName());
const testColl = testDB.getCollection("test");

let reply = assert.commandWorked(testColl.insert({_id: 0}, {"writeConcern": {"w": "majority"}}));
jsTestLog(`Inserted _id 0 into P1 ${reply.operationTime}`);

rollbackTest.transitionToRollbackOperations();
reply = assert.commandWorked(testColl.insert({_id: 1}, {"writeConcern": {"w": 1}}));
jsTestLog(`Inserted _id 1 into P1 ${reply.operationTime}`);

jsTestLog("Isolating P1 from tiebreaker");
P1.disconnect([T]);

jsTestLog("Reconnecting P2 to the tiebreaker");
P2.reconnect([T]);

jsTestLog("Step up P2");
assert.soonNoExcept(() => {
    const res = P2.adminCommand({replSetStepUp: 1});
    return res.ok;
}, "Failed to step up P2", ReplSetTest.kDefaultTimeoutMS);
checkLog.contains(P2, "transition to primary complete; database writes are now permitted");
jsTestLog("P2 stepped up");

reply = assert.commandWorked(testDB.runCommand({delete: "test", deletes: [{q: {}, limit: 0}]}));
jsTestLog(`Deleted from P1 at ${reply.operationTime}`);

// Ensure P1's lastApplied > P2's, even if P1's set-up entry was written at the same timestamp as
// P2's delete timestamp.
assert.soon(() => {
    testDB.runCommand({insert: "otherCollection", documents: [{}]});
    function lastApplied(node) {
        const reply = assert.commandWorked(node.adminCommand({replSetGetStatus: 1}));
        return reply.optimes.appliedOpTime.ts;
    }
    const P1applied = lastApplied(P1);
    const P2applied = lastApplied(P2);
    jsTestLog(`P1 lastApplied ${P1applied}, P2 lastApplied ${P2applied}`);
    return timestampCmp(P1applied, P2applied) > 0;
}, "P1's lastApplied never surpassed P2's");

jsTestLog("Reconnecting P1 to P2, so P1 rolls back");
P1.reconnect([P2]);
checkLog.contains(P1, "Rollback using the 'rollbackViaRefetch' method");
checkLog.contains(P1, "Finding the Common Point");
checkLog.contains(
    P1,
    "We cannot transition to SECONDARY state because our 'lastApplied' optime is less than the" +
        " initial data timestamp and enableMajorityReadConcern = false");

reply = assert.commandWorked(
    P2.getDB(jsTestName()).runCommand({insert: "anotherCollection", documents: [{}]}));
jsTestLog(`Inserted into P2 at ${tojson(reply.operationTime)}`);

jsTestLog("Wait for P1 to enter SECONDARY");
waitForState(P1, ReplSetTest.State.SECONDARY);

// Both counts should be 1. If SERVER-48518 isn't fixed then itCount() = 3: _ids 0, 1, and 0 again!
jsTestLog("Check collection count");
let itCount = testColl.find().itcount();
let fastCount = testColl.count();
assert.eq(itCount,
          fastCount,
          `count: ${fastCount}, itCount: ${itCount}, data: ${tojson(testColl.find().toArray())}`);

jsTestLog("Check succeeded. Ending test.");
P1.reconnect([T]);
restartServerReplication(T);
rst.stopSet();
}());
