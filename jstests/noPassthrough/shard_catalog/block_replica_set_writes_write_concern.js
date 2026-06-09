/**
 * Tests that the blockReplicaSetWrites command waits for the critical-section write to be majority
 * committed before returning. Replication is paused on the secondaries so the write cannot reach a
 * majority; therefore, the command must remain blocked until replication is resumed.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 *   uses_parallel_shell,
 *   featureFlagBlockReplicaSetWrites,
 * ]
 */
import {disableReplicaSetWriteBlock, enableReplicaSetWriteBlock} from "jstests/libs/block_replica_set_writes_utils.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, afterEach, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {restartReplicationOnSecondaries, stopReplicationOnSecondaries} from "jstests/libs/write_concern_util.js";

function isBlockReplicaSetWritesRunning(adminDB) {
    return (
        adminDB
            .aggregate([{$currentOp: {allUsers: true}}, {$match: {"command.blockReplicaSetWrites": {$exists: true}}}])
            .itcount() > 0
    );
}

describe("Test that blockReplicaSetWrites waits for majority write concern", function () {
    before(function () {
        this.rst = new ReplSetTest({nodes: 3});
        this.rst.startSet();
        this.rst.initiate();
    });

    afterEach(function () {
        // Make sure replication is running again so cleanup writes can be majority committed.
        restartReplicationOnSecondaries(this.rst);
        // Use getPrimary() rather than this.primaryAdminDB so that cleanup targets the current
        // primary even if a stepdown occurred during the test.
        disableReplicaSetWriteBlock(this.rst.getPrimary().getDB("admin"), "InsufficientDiskSpace" /* reason */);
    });

    after(function () {
        this.rst.stopSet();
    });

    it("Test that blockReplicaSetWrites blocks until the critical-section write is majority committed", function () {
        const primary = this.rst.getPrimary();
        const primaryAdminDB = primary.getDB("admin");
        // Pause replication on the secondaries so a write can never reach a majority. Pass false
        // to avoid changing the cluster's default write concern — blockReplicaSetWrites uses an
        // explicit majority write concern internally, not the cluster default, so changing it is
        // unnecessary and would leak a {w:1} default into the subsequent it() blocks.
        stopReplicationOnSecondaries(this.rst, false /* changeReplicaSetDefaultWCToLocal */);

        // Run the enable command in a parallel shell; it must block waiting for majority commit.
        const awaitShell = startParallelShell(() => {
            assert.commandWorked(
                db.getSiblingDB("admin").runCommand({
                    blockReplicaSetWrites: 1,
                    enabled: true,
                    allowDeletions: false,
                    reason: "InsufficientDiskSpace",
                }),
            );
        }, primary.port);

        // The command should appear in currentOp and stay there, blocked on write concern.
        assert.soon(
            () => isBlockReplicaSetWritesRunning(primaryAdminDB),
            "blockReplicaSetWrites command did not start running",
        );

        // Give it a moment and confirm it is still blocked (has not returned).
        sleep(2000);
        assert(
            isBlockReplicaSetWritesRunning(primaryAdminDB),
            "blockReplicaSetWrites returned before the write was majority committed",
        );

        // Resume replication; the majority wait can now be satisfied and the command completes.
        restartReplicationOnSecondaries(this.rst);
        awaitShell();

        // Check that writes are rejected after the command completes.
        assert.commandFailedWithCode(
            primaryAdminDB.getSiblingDB("testDB").testColl.insert({x: 1}),
            ErrorCodes.ReplicaSetWritesBlocked,
        );
    });

    it("Test that the write block remains enabled and active after a primary re-election", function () {
        const primary = this.rst.getPrimary();
        const primaryAdminDB = primary.getDB("admin");

        // Enable the write block on the current primary and confirm that user writes are rejected before the stepdown.
        enableReplicaSetWriteBlock(primaryAdminDB, false /* allowDeletions */, "InsufficientDiskSpace" /* reason */);
        assert.commandFailedWithCode(
            primaryAdminDB.getSiblingDB("testDB").testColl.insert({x: 1}),
            ErrorCodes.ReplicaSetWritesBlocked,
        );

        // Step down and wait for a new primary to be elected.
        let stepDownRes;
        try {
            // Note: adminCommand() only throws on network errors. A failed stepdown (e.g.
            // ExceededTimeLimit) returns {ok:0} rather than throwing, so the result must be
            // checked separately to avoid the test passing with no re-election.
            stepDownRes = primaryAdminDB.adminCommand({replSetStepDown: 60});
        } catch (e) {
            // Expected — the primary closes connections on successful stepdown.
        }
        if (stepDownRes !== undefined) {
            assert.commandWorked(stepDownRes, "replSetStepDown failed without stepping down");
        }
        this.rst.awaitNodesAgreeOnPrimary();
        const newPrimaryAdminDB = this.rst.getPrimary().getDB("admin");
        assert.neq(
            newPrimaryAdminDB.getMongo().host,
            primary.host,
            "Expected a different node to be elected as primary after stepdown",
        );

        // Check that user writes are still rejected on the new primary.
        assert.commandFailedWithCode(
            newPrimaryAdminDB.getSiblingDB("testDB").testColl.insert({x: 1}),
            ErrorCodes.ReplicaSetWritesBlocked,
        );
    });
});
