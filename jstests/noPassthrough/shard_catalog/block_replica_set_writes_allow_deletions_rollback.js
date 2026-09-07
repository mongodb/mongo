/**
 * Tests that an in-place allowDeletions update to config.replica_set_writes_critical_section that
 * has not majority-committed is rolled back and that in-memory RSWB state is restored from the
 * durable document while that document remains present.
 *
 * The blockReplicaSetWrites command waits for majority after the local CS write, so while the
 * rollback node is partitioned a command-driven update would hang on write concern. The update
 * below writes the same field with w:1 so OpObserver still commits the in-memory policy change,
 * matching a command that has completed its local write but not yet its majority wait.
 *
 * RollbackTest's ensure-rollback insert targets a DB that would be rejected under RSWB. This test
 * points that insert at an internal DB (admin) so inserts are permitted without deleting or
 * releasing the critical section document.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 *   requires_mongobridge,
 *   featureFlagBlockReplicaSetWrites,
 * ]
 */
import {enableReplicaSetWriteBlock} from "jstests/libs/block_replica_set_writes_utils.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {RollbackTest} from "jstests/replsets/libs/rollback_test.js";

const kReason = "InsufficientDiskSpace";
const kCsCollName = "replica_set_writes_critical_section";

function assertDeletionsBlockedOnNode(node) {
    const replStatus = node.getDB("admin").serverStatus().repl;
    assert.eq(
        false,
        replStatus.replicaSetWritesBlockAllowDeletions,
        "replicaSetWritesBlockAllowDeletions should be false when deletions are blocked",
    );
    const csDoc = node.getDB("config")[kCsCollName].findOne();
    assert(csDoc, "Expected a durable replica set writes critical section document");
    assert.eq(true, csDoc.enabled, "Expected write blocking to remain enabled");
    assert.eq(false, csDoc.allowDeletions, "Expected durable allowDeletions to be false");
}

function assertDeletionsBlocked(node, coll) {
    assertDeletionsBlockedOnNode(node);
    assert.commandFailedWithCode(coll.remove({_id: 1}), ErrorCodes.ReplicaSetWritesBlocked);
}

describe("Rollback of an in-place allowDeletions update for replica set write blocking", function () {
    before(function () {
        // Use an internal DB for RollbackTest's ensure-rollback insert so RSWB does not reject it.
        this.rollbackTest = new RollbackTest(jsTestName(), undefined, undefined, {
            ensureRollbackDbName: "admin",
        });
    });

    after(function () {
        if (!this.rollbackTest) {
            return;
        }
        // stop() awaits replication. If the test failed while partitioned, reconnect first so
        // teardown cannot hang forever waiting on an isolated secondary. Skip consistency checks
        // when we never reached steady state (transitionToSteadyStateOperations already checks on
        // the success path).
        const rst = this.rollbackTest.getTestFixture();
        rst.nodes.forEach((node) => {
            try {
                node.reconnect(rst.nodes);
            } catch (e) {
                // Best-effort; stop() still tears the set down.
            }
        });
        this.rollbackTest.stop(undefined, true /* skipDataConsistencyCheck */);
    });

    it("restores allowDeletions:false in memory after rolling back an update to true", function () {
        let primary = this.rollbackTest.getPrimary();
        const testDB = primary.getDB(jsTestName());
        const coll = testDB.coll;

        // Seed a document and enable the block with deletions disallowed at the common point.
        assert.commandWorked(coll.insert({_id: 1, a: 1}, {writeConcern: {w: "majority"}}));
        enableReplicaSetWriteBlock(primary.getDB("admin"), false /* allowDeletions */, kReason);
        assertDeletionsBlocked(primary, coll);

        this.rollbackTest.transitionToRollbackOperations();

        // Locally update allowDeletions to true (not majority-committed while partitioned).
        // The durable CS document remains present for the rest of the test.
        const rollbackNode = this.rollbackTest.getPrimary();
        assert.commandWorked(
            rollbackNode
                .getDB("config")
                [kCsCollName].update({}, {$set: {allowDeletions: true}}, {writeConcern: {w: 1}}),
        );

        // Confirm the OpObserver applied the new policy before histories diverge further.
        // Inserts/updates remain blocked; only deletions are permitted under this policy.
        assert.eq(
            true,
            rollbackNode.getDB("admin").serverStatus().repl.replicaSetWritesBlockAllowDeletions,
            "In-memory allowDeletions should flip to true after the local update commits",
        );
        assert.commandWorked(rollbackNode.getDB(jsTestName()).coll.remove({_id: 1}));
        assert(
            rollbackNode.getDB("config")[kCsCollName].findOne(),
            "CS document must remain present so rollback recovers an in-place allowDeletions update",
        );

        this.rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
        this.rollbackTest.transitionToSyncSourceOperationsDuringRollback();
        this.rollbackTest.transitionToSteadyStateOperations();

        // The node that performed the rolled-back update must have restored in-memory policy from
        // the durable document (it may no longer be primary).
        assertDeletionsBlockedOnNode(rollbackNode);

        // Current primary must also enforce deletions blocked.
        primary = this.rollbackTest.getPrimary();
        assertDeletionsBlocked(primary, primary.getDB(jsTestName()).coll);
    });
});
