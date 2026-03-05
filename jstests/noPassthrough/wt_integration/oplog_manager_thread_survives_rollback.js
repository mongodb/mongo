/**
 * Reproduces a scenario where:
 * - A long-running agg holds a CollectionCatalog snapshot on a node.
 * - That node goes through rollback (closeCatalog/openCatalog, OplogManager stop/start).
 * - The node is later stepped up to primary.
 * - The read resumes, gets InterruptedDueToReplStateChange, and releases the old RS.
 *
 * Prior to SERVER-119964, this resulted in the oplog manager thread exiting,
 * and oplog visibility would never advance. This made majority writes hang
 * indefinitely.
 *
 * The intent registry prevents proceeding through a rollback while a
 * pre-rollback aggregation is hung, which breaks this test but also prevents
 * the bug from occurring.
 *
 * @tags: [
 *   requires_replication,
 *   requires_mongobridge,
 *   requires_persistence,
 *   featureFlagIntentRegistration_incompatible
 * ]
 */

import {RollbackTest} from "jstests/replsets/libs/rollback_test.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

const dbName = jsTestName();
const collName = "testColl";

const rollbackTest = new RollbackTest();

jsTestLog.info("Populate database and wait for steady state replication");
const primary = rollbackTest.getPrimary();
const primaryDB = primary.getDB(dbName);
const coll = primaryDB.getCollection(collName);

assert.commandWorked(
    coll.insert(
        Array.from({length: 10}, (_, i) => ({_id: i, x: i})),
        {writeConcern: {w: "majority"}},
    ),
);

rollbackTest.awaitReplication();

// Enable debug logging for storage to check for oplog manager thread not stopping
assert.commandWorked(primary.setLogLevel(1, "storage"));

const rollbackNode = rollbackTest.transitionToRollbackOperations();

/**
 * Start an aggregation on the rollback node and hang it during yield via
 * setYieldAllLocksHang. This will release any locks but _not_ refresh the
 * CollectionCatalog shared_ptr.
 */
jsTestLog.info("Enabling setYieldAllLocksHang on rollback node and starting aggregation");

// Force very frequent yields on the rollback node.
assert.commandWorked(rollbackNode.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}));

const hangDuringYield = configureFailPoint(rollbackNode, "setYieldAllLocksHang", {
    namespace: `${dbName}.${collName}`,
    // Important: don’t call checkForInterrupt at the hang site; we want to
    // ignore repl state changes until the agg resumes.
    checkForInterruptAfterHang: false,
});

const awaitAgg = startParallelShell(
    funWithArgs(
        (dbName, collName) => {
            // This aggregation runs on the rollback node. It will:
            // - Start a lock-free read / consistent catalog + storage snapshot.
            // - Yield frequently; on each yield, setYieldAllLocksHang may fire and
            //   block the op while locks are dropped but the snapshot is pinned.
            // When it finally resumes after rollback + step-up, it should see that the node's
            // replication state has changed and fail with InterruptedDueToReplStateChange
            // (or a network error, depending on timing).
            try {
                const res = db.getSiblingDB(dbName).runCommand({
                    aggregate: collName,
                    pipeline: [
                        {
                            $match: {
                                $expr: {
                                    $in: ["$_id", [...Array(10).keys()]],
                                },
                            },
                        },
                    ],
                    cursor: {},
                });
                jsTestLog.info("Aggregation on rollback node completed with: " + tojson(res));
            } catch (e) {
                jsTestLog.info("Aggregation on rollback node failed with exception: " + tojson(e));
            }
        },
        dbName,
        collName,
    ),
    rollbackNode.port,
);

jsTestLog.info("Wait for aggregation to reach the setYieldAllLocksHang failpoint");
hangDuringYield.wait();

rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();
rollbackTest.transitionToSteadyStateOperations();
rollbackTest.stepUpNode(primary);

/**
 * Unblock the paused aggregation.
 *
 * Turning off the failpoint lets the aggregation continue. Because the node's
 * replication state changed (went through rollback and then was stepped up),
 * the command should be interrupted due to repl state change at some later interrupt
 * point. When the OperationContext unwinds, it releases its CollectionCatalog snapshot,
 * which should drop the last reference to RS_old, run ~Oplog() on RS_old, and
 * call WiredTigerOplogManager::stop() a second time in the "RS_new" state.
 */
jsTestLog.info("Turning off setYieldAllLocksHang so hung aggregation can resume");
hangDuringYield.off();
awaitAgg();

jsTestLog.info("Wait for oplog manager thread to not exit");
checkLog.containsJson(rollbackNode, 11996400, {});

jsTestLog.info("Perform a majority write, which would hang forever if the thread wasn't running");
assert.commandWorked(
    coll.insert(
        Array.from({length: 10}, (_, i) => ({_id: i + 10, x: i})),
        {writeConcern: {w: "majority"}},
    ),
);

rollbackTest.stop();
