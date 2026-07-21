/**
 * Tests sharding specific functionality of the blockReplicaSetWrites command, including
 * interaction with cluster-wide setUserWriteBlockMode (global user write blocking). Replica
 * set specific functionality aspects of this command should be checked on
 * jstests/core/administrative/block_replica_set_writes.js instead.
 *
 * @tags: [
 *   requires_sharding,
 *   requires_persistence,
 *   uses_parallel_shell,
 *   does_not_support_stepdowns,
 *   assumes_balancer_off,
 *   featureFlagBlockReplicaSetWrites,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {
    disableReplicaSetWriteBlock,
    enableReplicaSetWriteBlock,
} from "jstests/libs/block_replica_set_writes_utils.js";
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {isEnterpriseShell, runEncryptedTest} from "jstests/fle2/libs/encrypted_client_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {checkLog} from "src/mongo/shell/check_log.js";
import {PersistenceProviderUtil} from "jstests/libs/server-rss/persistence_provider_util.js";

// Runs an autoCompact command, retrying while it reports the transient ObjectIsBusy (the previous
// command's signal has not been consumed yet) and asserting it ultimately succeeds.
function runAutoCompactRetryingBusy(adminDB, cmdObj, timeoutMsg, workedMsg) {
    assert.soon(() => {
        const res = adminDB.runCommand(cmdObj);
        if (res.code === ErrorCodes.ObjectIsBusy) return false;
        assert.commandWorked(res, workedMsg);
        return true;
    }, timeoutMsg);
}

// Confirms background auto-compact is running with the expected options.
// Re-requesting the same options is a silent no-op. We therefore probe with a deliberately different
// freeSpaceTargetMB: while auto-compact is running, the probe is rejected and the active
// options are reported in the error message. A transient ObjectIsBusy
// (the previous command's signal not yet consumed) is retried.
function assertAutoCompactRunningWith(adminDB, expectedMB) {
    const probeMB = expectedMB + 1;
    assert.soon(() => {
        const res = adminDB.runCommand({autoCompact: true, freeSpaceTargetMB: probeMB});
        if (res.code === ErrorCodes.ObjectIsBusy) return false;
        assert.eq(
            res.code,
            ErrorCodes.AlreadyInitialized,
            "auto-compact reconfigure should be rejected because it is already running",
            {res},
        );
        assert(
            res.errmsg.includes(`freeSpaceTargetMB: ${expectedMB}`),
            "autoCompact error should report the active options",
            {res},
        );
        return true;
    }, `Timed out confirming auto-compact is running with expected options`);
}

describe("Test blockReplicaSetWrites command on shard replica sets in a sharded cluster", function () {
    before(function () {
        this.st = new ShardingTest({shards: 2, rs: {nodes: 2}});
        this.shard0Primary = this.st.rs0.getPrimary();
        this.shard0PrimaryAdminDB = this.shard0Primary.getDB("admin");
        this.shard0PrimaryConfigDB = this.shard0Primary.getDB("config");
    });

    beforeEach(function () {
        this.testDBName = jsTestName();
        this.shardedCollName = "testShardedColl";
        this.nns = `${this.testDBName}.${this.shardedCollName}`;
        this.extraDatabasesToDrop = [];

        assert.commandWorked(
            this.st.s.adminCommand({
                enablesharding: this.testDBName,
                primaryShard: this.st.shard0.shardName,
            }),
        );
        this.testDB = this.st.s.getDB(this.testDBName);
        this.testShardedColl = this.testDB.getCollection(this.shardedCollName);
    });

    afterEach(function () {
        // Disable replica set and global user write block (if not previously enabled the operation is a no-op).
        disableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            "InsufficientDiskSpace" /* reason */,
        );
        disableReplicaSetWriteBlock(
            this.st.rs1.getPrimary().getDB("admin"),
            "InsufficientDiskSpace" /* reason */,
        );
        assert.commandWorked(
            this.st.s.adminCommand({
                setUserWriteBlockMode: 1,
                global: false,
                reason: "DiskUseThresholdExceeded",
            }),
        );

        assert.soon(() => {
            const res = this.shard0PrimaryAdminDB.runCommand({autoCompact: false});
            return res.code !== ErrorCodes.ObjectIsBusy;
        }, "Timed out disabling autoCompact in afterEach");

        for (const dbName of this.extraDatabasesToDrop) {
            assert.commandWorked(this.st.s.getDB(dbName).dropDatabase());
        }

        // Drop test database.
        assert.commandWorked(this.testDB.dropDatabase());
    });

    after(function () {
        this.st.stop();
    });

    it("Test blockReplicaSetWrites command counter increments on enable", function () {
        // Enable per-shard write blocking on shard0.
        enableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );

        // Check replica set write block, reason, and command counter metrics when blockReplicaSetWrites is enabled.
        let replStatus = assert.commandWorked(this.shard0PrimaryAdminDB.serverStatus()).repl;
        assert.eq(
            replStatus.replicaSetWritesBlock,
            2,
            "replicaSetWritesBlock metric should be 2 (Enabled)",
        );
        assert.eq(
            replStatus.replicaSetWritesBlockReason,
            0,
            "replicaSetWritesBlockReason metric should be 0 (InsufficientDiskSpace)",
        );
        assert.eq(
            replStatus.replicaSetWritesBlockCounters.InsufficientDiskSpace,
            1,
            "repl.replicaSetWritesBlockCounters counter for InsufficientDiskSpace should be 1",
        );

        // Disable write blocking on shard0.
        disableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            "InsufficientDiskSpace" /* reason */,
        );

        // Check replica set write block, reason, and command counter when blockReplicaSetWrites is disabled.
        replStatus = assert.commandWorked(this.shard0PrimaryAdminDB.serverStatus()).repl;
        assert.eq(
            replStatus.replicaSetWritesBlock,
            1,
            "replicaSetWritesBlock metric should be 1 (Disabled)",
        );
        assert(
            !replStatus.hasOwnProperty("replicaSetWritesBlockReason"),
            "replicaSetWritesBlockReason should be absent when replica set write block is disabled",
        );
        assert.eq(
            replStatus.replicaSetWritesBlockCounters.InsufficientDiskSpace,
            1,
            "repl.replicaSetWritesBlockCounters counter for InsufficientDiskSpace should not change on disable",
        );
    });

    it("Test blocked inserts, updates, and deletes counters increment after blockReplicaSetWrites is enabled", function () {
        assert.commandWorked(this.testShardedColl.insert({x: 0}));

        // Enable per-shard write blocking on shard0.
        enableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );

        // Check blocked inserts, updates, and deletes are blocked and counters are incremented.
        let replStatus = assert.commandWorked(this.shard0PrimaryAdminDB.serverStatus()).repl;
        assert(replStatus.hasOwnProperty("replicaSetWritesBlockRejected"));
        const insertsAfterEnable = replStatus.replicaSetWritesBlockRejected.inserts;
        const updatesAfterEnable = replStatus.replicaSetWritesBlockRejected.updates;
        const deletesAfterEnable = replStatus.replicaSetWritesBlockRejected.deletes;

        assert.commandFailedWithCode(
            this.testShardedColl.insert({x: 1}),
            ErrorCodes.ReplicaSetWritesBlocked,
        );
        assert.commandFailedWithCode(
            this.testShardedColl.insert({x: 2}),
            ErrorCodes.ReplicaSetWritesBlocked,
        );
        assert.commandFailedWithCode(
            this.testShardedColl.update({x: 0}, {$set: {y: 1}}),
            ErrorCodes.ReplicaSetWritesBlocked,
        );
        assert.commandFailedWithCode(
            this.testShardedColl.remove({x: 0}),
            ErrorCodes.ReplicaSetWritesBlocked,
        );

        replStatus = assert.commandWorked(this.shard0PrimaryAdminDB.serverStatus()).repl;
        assert.eq(
            replStatus.replicaSetWritesBlockRejected.inserts,
            insertsAfterEnable + 2,
            "Each blocked insert should increment repl.replicaSetWritesBlockRejected.inserts",
        );
        assert.eq(
            replStatus.replicaSetWritesBlockRejected.updates,
            updatesAfterEnable + 1,
            "Each blocked update should increment repl.replicaSetWritesBlockRejected.updates",
        );
        assert.eq(
            replStatus.replicaSetWritesBlockRejected.deletes,
            deletesAfterEnable + 1,
            "Each blocked delete should increment repl.replicaSetWritesBlockRejected.deletes",
        );
    });

    it("Test user writes respect both setUserWriteBlockMode and blockReplicaSetWrites on a shard", function () {
        // Insert a document on shard0.
        assert.commandWorked(this.testShardedColl.insert({x: 1}));

        // Enable global user write blocking and check that insert fails with UserWritesBlocked error.
        assert.commandWorked(
            this.st.s.adminCommand({
                setUserWriteBlockMode: 1,
                global: true,
                reason: "DiskUseThresholdExceeded",
            }),
        );
        assert.commandFailedWithCode(
            this.testShardedColl.insert({x: 2}),
            ErrorCodes.UserWritesBlocked,
        );

        // Enable per-shard write blocking on shard0 and check that insert still fails with
        // UserWritesBlocked error.
        enableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );
        assert.commandFailedWithCode(
            this.testShardedColl.insert({x: 3}),
            ErrorCodes.UserWritesBlocked,
        );

        // Disable global user write blocking and check that insert still fails with ReplicaSetWritesBlocked error.
        assert.commandWorked(
            this.st.s.adminCommand({
                setUserWriteBlockMode: 1,
                global: false,
                reason: "DiskUseThresholdExceeded",
            }),
        );
        assert.commandFailedWithCode(
            this.testShardedColl.insert({x: 4}),
            ErrorCodes.ReplicaSetWritesBlocked,
            "Expected ReplicaSetWritesBlocked error after cluster user write block is cleared",
        );

        // Disable write blocking on shard0 and check that insert succeeds.
        disableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            "InsufficientDiskSpace" /* reason */,
        );
        assert.commandWorked(this.testShardedColl.insert({x: 5}));
    });

    it("Test writes on a shard without blockReplicaSetWrites still allow insert, update, and delete", function () {
        // Shard the collection and insert some documents.
        assert.commandWorked(this.testDB.createCollection(this.shardedCollName));
        assert.commandWorked(this.st.s.adminCommand({shardCollection: this.nns, key: {x: 1}}));
        assert.commandWorked(this.testShardedColl.insert({x: -1, y: "a"}));
        assert.commandWorked(this.testShardedColl.insert({x: 1, y: "b"}));

        // Split the chunk and move it to shard1.
        assert.commandWorked(this.st.s.adminCommand({split: this.nns, middle: {x: 0}}));
        assert.commandWorked(
            this.st.s.adminCommand({
                moveChunk: this.nns,
                find: {x: 0},
                to: this.st.shard1.shardName,
            }),
        );

        // Drop mongos routing cache so that it is forced to load the updated catalog metadata, which
        // guarantees operations are routed to the correct shard.
        assert.commandWorked(this.st.s.adminCommand({flushRouterConfig: this.testDBName}));

        // Enable per-shard write blocking on shard0.
        enableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );

        // Check that insert, update, and delete operations routed to shard0 fail with ReplicaSetWritesBlocked error.
        assert.commandFailedWithCode(
            this.testShardedColl.insert({x: -2, y: "c"}),
            ErrorCodes.ReplicaSetWritesBlocked,
        );
        assert.commandFailedWithCode(
            this.testShardedColl.update({x: -1}, {$set: {y: "d"}}),
            ErrorCodes.ReplicaSetWritesBlocked,
        );
        assert.commandFailedWithCode(
            this.testShardedColl.remove({x: -1}),
            ErrorCodes.ReplicaSetWritesBlocked,
        );
        assert.eq(1, this.testShardedColl.find({x: -1, y: "a"}).itcount());

        // Check that insert, update, and delete operations routed to shard1 succeed.
        assert.commandWorked(this.testShardedColl.insert({x: 2, y: "c"}));
        assert.commandWorked(this.testShardedColl.update({x: 1}, {$set: {y: "d"}}));
        assert.commandWorked(this.testShardedColl.remove({x: 2}));
        assert.eq(1, this.testShardedColl.find({x: 1, y: "d"}).itcount());
        assert.eq(0, this.testShardedColl.find({x: 2}).itcount());
    });

    it("Test user deletions are blocked when allowDeletions is false and allowed when allowDeletions is true", function () {
        const testColl = this.testDB.getCollection("userDeletionTestColl");

        assert.commandWorked(testColl.insert({_id: 1}));
        assert.commandWorked(testColl.insert({_id: 2}));

        // Enable write block with allowDeletions: false — user deletes should be rejected.
        enableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );
        assert.commandFailedWithCode(testColl.remove({_id: 1}), ErrorCodes.ReplicaSetWritesBlocked);
        assert.eq(2, testColl.count(), "Both documents should remain while deletions are blocked");

        // Disable write block and re-enable with allowDeletions: true — user deletes should succeed.
        disableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            "InsufficientDiskSpace" /* reason */,
        );

        enableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            true /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );
        assert.commandWorked(testColl.remove({_id: 1}));
        assert.eq(
            1,
            testColl.count(),
            "Document should have been deleted when allowDeletions is true",
        );

        // Inserts and updates should still be blocked when allowDeletions is true.
        assert.commandFailedWithCode(testColl.insert({_id: 3}), ErrorCodes.ReplicaSetWritesBlocked);
        assert.commandFailedWithCode(
            testColl.update({_id: 2}, {$set: {x: 1}}),
            ErrorCodes.ReplicaSetWritesBlocked,
        );
    });

    it("Test range deletion on donor is blocked when allowDeletions is false and allowed when allowDeletions is true", function () {
        const shard0LocalColl = this.st.shard0
            .getDB(this.testDBName)
            .getCollection(this.shardedCollName);

        // Shard the collection and insert some documents.
        assert.commandWorked(this.testDB.createCollection(this.shardedCollName));
        assert.commandWorked(this.st.s.adminCommand({shardCollection: this.nns, key: {x: 1}}));
        assert.commandWorked(this.testShardedColl.insert({x: -1}));
        assert.commandWorked(this.testShardedColl.insert({x: 1}));

        // Suspend range deletion on shard0.
        const suspendRangeDel = configureFailPoint(this.st.shard0, "suspendRangeDeletion");

        // Split the chunk and move it to shard1.
        assert.commandWorked(this.st.s.adminCommand({split: this.nns, middle: {x: 0}}));
        assert.commandWorked(
            this.st.s.adminCommand({
                moveChunk: this.nns,
                find: {x: 0},
                to: this.st.shard1.shardName,
            }),
        );

        // Check that the orphaned document is still present on shard0.
        assert.eq(
            1,
            shard0LocalColl.find({x: 1}).itcount(),
            "Expected donor shard to retain an orphaned copy until range deletion runs",
        );

        // Enable per-shard write blocking on shard0 with allowDeletions set to false (i.e., all deletes blocked).
        enableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );

        // Let the range deleter run while deletions are blocked and check that the range deleter
        // fails to establish a cursor to delete the range with ReplicaSetWritesBlocked error.
        suspendRangeDel.off();
        checkLog.containsJson(
            this.st.shard0,
            6180602,
            {error: /ReplicaSetWritesBlocked/},
            60 * 1000,
        );

        // The orphan should still be present since range deletion was blocked.
        assert.eq(
            1,
            shard0LocalColl.find({x: 1}).itcount(),
            "Expected orphan to still be present after range deletion was blocked",
        );

        // Disable write block, then re-enable with allowDeletions set to true (i.e., all deletes allowed).
        disableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            "InsufficientDiskSpace" /* reason */,
        );

        enableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            true /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );

        // Check that range deletion now completes since allowDeletions is true.
        assert.soon(
            () => shard0LocalColl.find({x: 1}).itcount() === 0,
            "Expected donor orphan to be removed by range deleter when allowDeletions is true",
            5 * 60 * 1000,
            200,
        );
    });

    it("Test that compact on a shard is guarded by the allowDeletions flag of blockReplicaSetWrites", function () {
        // Skip test function if the architecture does not support auto-compact operations.
        if (
            PersistenceProviderUtil.allNodesHavePropertyWithValue(
                this.shard0Primary,
                "supportsLocalCollections",
                false,
            )
        ) {
            jsTest.log.info(
                "Skipping 'Test that compact on a shard is guarded by the allowDeletions flag of blockReplicaSetWrites' test function because local collections are not supported",
            );
            return;
        }
        assert.commandWorked(this.testShardedColl.insert({x: 1}));
        const shard0TestDB = this.shard0Primary.getDB(this.testDBName);

        // Check that with allowDeletions:false, compact is rejected.
        enableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );
        assert.commandFailedWithCode(
            shard0TestDB.runCommand({compact: this.shardedCollName, force: true}),
            ErrorCodes.ReplicaSetWritesBlocked,
        );

        // Disable write blocking.
        disableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            "InsufficientDiskSpace" /* reason */,
        );

        // Check that with allowDeletions:true, compact is permitted.
        enableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            true /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );
        assert.commandWorked(shard0TestDB.runCommand({compact: this.shardedCollName, force: true}));

        // Disable write blocking entirely and verify compact still succeeds.
        disableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            "InsufficientDiskSpace" /* reason */,
        );
        assert.commandWorked(shard0TestDB.runCommand({compact: this.shardedCollName, force: true}));
    });

    it("Test that autoCompact on a shard is guarded by the allowDeletions flag while disabling is always allowed", function () {
        // Skip test function if the architecture does not support auto-compact operations.
        if (
            PersistenceProviderUtil.allNodesHavePropertyWithValue(
                this.shard0Primary,
                "supportsLocalCollections",
                false,
            )
        ) {
            jsTest.log.info(
                "Skipping 'Test that autoCompact on a shard is guarded by the allowDeletions flag of blockReplicaSetWrites' test function because local collections are not supported",
            );
            return;
        }

        const shard0AdminDB = this.shard0PrimaryAdminDB;

        // Check that with allowDeletions:false, auto-compact is rejected.
        enableReplicaSetWriteBlock(
            shard0AdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );
        assert.commandFailedWithCode(
            shard0AdminDB.runCommand({autoCompact: true}),
            ErrorCodes.ReplicaSetWritesBlocked,
        );
        assert.commandWorked(shard0AdminDB.runCommand({autoCompact: false}));

        // Disable write blocking.
        disableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            "InsufficientDiskSpace" /* reason */,
        );

        // Check that with allowDeletions:true, auto-compact is permitted.
        enableReplicaSetWriteBlock(
            shard0AdminDB,
            true /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );
        runAutoCompactRetryingBusy(
            shard0AdminDB,
            {autoCompact: true},
            "Timed out waiting to enable autoCompact",
        );

        // Leave background compaction off.
        runAutoCompactRetryingBusy(
            shard0AdminDB,
            {autoCompact: false},
            "Timed out waiting to disable autoCompact",
        );
    });

    it("Test that auto-compact running before write block is enabled is stopped when the write block is enabled and restored when the write block is released", function () {
        if (
            PersistenceProviderUtil.allNodesHavePropertyWithValue(
                this.shard0Primary,
                "supportsLocalCollections",
                false,
            )
        ) {
            jsTest.log.info("Skipping auto-compact restore test: local collections not supported");
            return;
        }

        const adminDB = this.shard0PrimaryAdminDB;
        const freeSpaceTargetMB = 750;

        // Enable auto-compact with a custom freeSpaceTargetMB so we can verify the exact
        // options are preserved after restore.
        runAutoCompactRetryingBusy(
            adminDB,
            {autoCompact: true, freeSpaceTargetMB},
            "Timed out enabling autoCompact before write block",
        );

        // Confirm auto-compact is running with the expected options before engaging the write
        // block.
        assertAutoCompactRunningWith(adminDB, freeSpaceTargetMB);

        // Engage write block with allowDeletions:false, auto-compact should be stopped.
        enableReplicaSetWriteBlock(adminDB, false /* allowDeletions */, "InsufficientDiskSpace");

        // Enabling auto-compact is rejected while the write block is active.
        assert.commandFailedWithCode(
            adminDB.runCommand({autoCompact: true}),
            ErrorCodes.ReplicaSetWritesBlocked,
            "autoCompact enable must be blocked while write block is active",
        );

        // Release the write block, auto-compact should be restored with the original options.
        disableReplicaSetWriteBlock(adminDB, "InsufficientDiskSpace");

        // After releasing the write block, auto-compact must be restored with the original options.
        assertAutoCompactRunningWith(adminDB, freeSpaceTargetMB);

        // Disable auto-compact.
        runAutoCompactRetryingBusy(
            adminDB,
            {autoCompact: false},
            "Timed out disabling autoCompact after write block release",
        );
    });

    it("Test that explicitly disabling auto-compact during a write block prevents restore on write block release", function () {
        if (
            PersistenceProviderUtil.allNodesHavePropertyWithValue(
                this.shard0Primary,
                "supportsLocalCollections",
                false,
            )
        ) {
            jsTest.log.info(
                "Skipping auto-compact explicit-disable test: local collections not supported",
            );
            return;
        }

        const adminDB = this.shard0PrimaryAdminDB;
        const freeSpaceTargetMB = 600;

        // Enable auto-compact with a custom freeSpaceTargetMB.
        runAutoCompactRetryingBusy(
            adminDB,
            {autoCompact: true, freeSpaceTargetMB},
            "Timed out enabling autoCompact",
        );

        // Engage write block with allowDeletions:false — auto-compact is stopped, options saved.
        enableReplicaSetWriteBlock(adminDB, false /* allowDeletions */, "InsufficientDiskSpace");

        // Explicitly disable auto-compact while the write block is active.
        // Disabling is always permitted and signals user intent to stop permanently,
        // which must override the automatic restore on write block release.
        runAutoCompactRetryingBusy(
            adminDB,
            {autoCompact: false},
            "Timed out disabling autoCompact during write block",
            "Disabling autoCompact must always be permitted",
        );

        // Release the write block. Because we explicitly disabled auto-compact, the saved options
        // must be discarded — auto-compact must NOT be automatically restored.
        disableReplicaSetWriteBlock(adminDB, "InsufficientDiskSpace");

        // Confirm auto-compact was not restored. Probe by enabling with a *different*
        // freeSpaceTargetMB: had it been wrongly restored (and were running), this reconfigure
        // would be rejected with AlreadyInitialized. Because it is stopped, enabling succeeds.
        assert.soon(() => {
            const res = adminDB.runCommand({
                autoCompact: true,
                freeSpaceTargetMB: freeSpaceTargetMB + 1,
            });
            if (res.code === ErrorCodes.ObjectIsBusy) return false;
            assert.neq(
                res.code,
                ErrorCodes.AlreadyInitialized,
                "autoCompact must not be running after explicit disable during write block",
            );
            assert.commandWorked(
                res,
                "autoCompact enable must succeed after explicit disable during write block",
            );
            return true;
        }, "Timed out confirming autoCompact was not restored after explicit disable");

        // Disable auto-compact.
        runAutoCompactRetryingBusy(
            adminDB,
            {autoCompact: false},
            "Timed out disabling autoCompact at end of test",
        );
    });

    it("Test that index builds on empty collections bypass the replica set write block", function () {
        const shard0LocalDB = this.st.shard0.getDB(this.testDBName);
        const emptyColl = shard0LocalDB.getCollection("emptyColl");
        const nonEmptyColl = shard0LocalDB.getCollection("nonEmptyColl");

        // Populate nonEmptyColl before enabling the write block.
        assert.commandWorked(nonEmptyColl.insert({_id: 1, a: 1}));

        enableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );

        // Index build on an empty collection should bypass the write block.
        assert.commandWorked(emptyColl.createIndex({a: 1}));
        assert.neq(
            null,
            emptyColl.getIndexes().find((idx) => idx.name === "a_1"),
            "Expected index to exist on empty collection after bypassing write block",
        );

        // Index build on a non-empty collection should still be blocked.
        assert.commandFailedWithCode(
            nonEmptyColl.createIndex({b: 1}),
            ErrorCodes.ReplicaSetWritesBlocked,
        );
    });

    it("Test that sharding an empty collection with a hashed shard key succeeds when blockReplicaSetWrites is enabled", function () {
        // Enable per-shard write blocking on shard0, the primary shard for this database.
        enableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );

        // Sharding with a hashed shard key builds a hashed index on the new (empty) collection.
        // Because the index build is on an empty collection, it bypasses the write block and
        // sharding succeeds.
        assert.commandWorked(
            this.st.s.adminCommand({shardCollection: this.nns, key: {x: "hashed"}}),
        );

        // Verify the hashed shard key index was created on shard0.
        const shard0Coll = this.shard0Primary
            .getDB(this.testDBName)
            .getCollection(this.shardedCollName);
        assert.neq(
            null,
            shard0Coll.getIndexes().find((idx) => idx.key && idx.key.x === "hashed"),
            "Expected hashed shard key index to exist after sharding under write blocking",
        );
    });

    it("Test that convertToCapped on a shard is blocked when blockReplicaSetWrites is enabled", function () {
        // Create an unsharded collection on the primary shard (shard0), where the block is enabled.
        const testColl = this.testDB.getCollection("convertToCappedColl");
        assert.commandWorked(testColl.insert({_id: 1}));

        // Enable write block with allowDeletions:false and check that convertToCapped is rejected.
        enableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );
        assert.commandFailedWithCode(
            this.testDB.runCommand({convertToCapped: "convertToCappedColl", size: 100000}),
            ErrorCodes.ReplicaSetWritesBlocked,
        );

        // Disable write block and check that convertToCapped succeeds.
        disableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            "InsufficientDiskSpace" /* reason */,
        );
        assert.commandWorked(
            this.testDB.runCommand({convertToCapped: "convertToCappedColl", size: 100000}),
        );
        assert(
            testColl.stats().capped,
            "Collection should be capped after convertToCapped succeeds",
        );
    });

    it("Test that new index builds on user collections are blocked when blockReplicaSetWrites is enabled", function () {
        const shard0LocalDB = this.st.shard0.getDB(this.testDBName);
        const testColl = shard0LocalDB.getCollection("testColl");
        assert.commandWorked(testColl.insert({_id: 1, a: 1}));

        enableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );

        // A new index build on a non-empty user collection should be rejected with ReplicaSetWritesBlocked.
        assert.commandFailedWithCode(
            testColl.createIndex({a: 1}),
            ErrorCodes.ReplicaSetWritesBlocked,
        );
        assert.eq(
            null,
            testColl.getIndexes().find((idx) => idx.name === "a_1"),
            "Expected index to not exist while writes are blocked",
        );

        // After disabling the write block, new index builds should succeed again.
        disableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            "InsufficientDiskSpace" /* reason */,
        );
        assert.commandWorked(testColl.createIndex({a: 1}));
        assert.neq(
            null,
            testColl.getIndexes().find((idx) => idx.name === "a_1"),
            "Expected index to exist after write block is disabled",
        );
    });

    it("Test that the replica set write block and the disk space monitor do not conflict on index builds", function () {
        const shard0LocalDB = this.st.shard0.getDB(this.testDBName);
        const emptyColl = shard0LocalDB.getCollection("emptyColl");
        const nonEmptyColl = shard0LocalDB.getCollection("nonEmptyColl");

        // Populate nonEmptyColl before enabling the write block so its index build actually runs.
        assert.commandWorked(nonEmptyColl.insert({_id: 1, a: 1}));

        // Simulate low available disk space on shard0 (below the default 500 MB threshold) so the
        // disk space monitor would reject index builds, then enable the replica set write block.
        const simulateDiskSpaceFp = configureFailPoint(
            this.shard0Primary,
            "simulateAvailableDiskSpace",
            {bytes: 450 * 1024 * 1024},
        );
        try {
            enableReplicaSetWriteBlock(
                this.shard0PrimaryAdminDB,
                false /* allowDeletions */,
                "InsufficientDiskSpace" /* reason */,
            );

            // An index build on an empty collection does not start a real build, so it is exempt
            // from both the write block and the disk space check and should succeed.
            assert.commandWorked(emptyColl.createIndex({a: 1}));
            assert.neq(
                null,
                emptyColl.getIndexes().find((idx) => idx.name === "a_1"),
                "Expected index to exist on empty collection with both write block and disk space monitor enabled",
            );

            // An index build on a non-empty collection would be rejected by each mechanism on its
            // own. With both enabled it must still fail cleanly with one of the expected errors.
            assert.commandFailedWithCode(
                nonEmptyColl.createIndex({b: 1}),
                [ErrorCodes.ReplicaSetWritesBlocked, ErrorCodes.OutOfDiskSpace],
                "Expected non-empty collection index build to fail while both mechanisms are enabled",
            );
            assert.eq(
                null,
                nonEmptyColl.getIndexes().find((idx) => idx.name === "b_1"),
                "Expected index to not exist while both mechanisms are enabled",
            );

            // Lifting only the write block must leave the disk space monitor still rejecting the
            // build, confirming the two mechanisms operate independently.
            disableReplicaSetWriteBlock(
                this.shard0PrimaryAdminDB,
                "InsufficientDiskSpace" /* reason */,
            );
            assert.commandFailedWithCode(
                nonEmptyColl.createIndex({b: 1}),
                ErrorCodes.OutOfDiskSpace,
                "Expected disk space monitor to still reject the index build after lifting the write block",
            );
        } finally {
            simulateDiskSpaceFp.off();
        }

        // With both mechanisms disabled the index build on the non-empty collection succeeds.
        assert.commandWorked(nonEmptyColl.createIndex({b: 1}));
        assert.neq(
            null,
            nonEmptyColl.getIndexes().find((idx) => idx.name === "b_1"),
            "Expected index to exist after both write block and disk space monitor are disabled",
        );
    });

    it("Test that new rename across databases is blocked when blockReplicaSetWrites is enabled", function () {
        const sourceCollName = "crossDbRenameTestColl";
        const targetDBName = this.testDBName + "_crossDbTarget";

        // The rename must be rejected with ReplicaSetWritesBlocked when issued directly against the shard's
        // replica set primary and through mongos. The two paths differ only in the connection used, so run
        // the same flow against both.
        for (const {label, conn} of [
            {label: "mongos", conn: this.st.s},
            {label: "shard's replica set primary", conn: this.shard0Primary},
        ]) {
            const sourceColl = conn.getDB(this.testDBName).getCollection(sourceCollName);
            const targetDB = conn.getDB(targetDBName);
            assert.commandWorked(sourceColl.insert({x: 1}));

            // Enable write blocking on shard0 and check that cross-database rename is
            // blocked.
            enableReplicaSetWriteBlock(
                this.shard0PrimaryAdminDB,
                false /* allowDeletions */,
                "InsufficientDiskSpace" /* reason */,
            );
            assert.commandFailedWithCode(
                conn.adminCommand({
                    renameCollection: `${this.testDBName}.${sourceCollName}`,
                    to: `${targetDBName}.${sourceCollName}`,
                }),
                ErrorCodes.ReplicaSetWritesBlocked,
                `Cross-DB rename issued via ${label} should be blocked`,
            );

            // Source collection should still exist after the blocked rename.
            assert.eq(
                1,
                sourceColl.find({x: 1}).itcount(),
                "Source collection should still exist after blocked cross-DB rename",
            );

            // Disable write blocking and check that cross-database rename succeeds.
            disableReplicaSetWriteBlock(
                this.shard0PrimaryAdminDB,
                "InsufficientDiskSpace" /* reason */,
            );
            assert.commandWorked(
                conn.adminCommand({
                    renameCollection: `${this.testDBName}.${sourceCollName}`,
                    to: `${targetDBName}.${sourceCollName}`,
                }),
            );

            assert.eq(
                1,
                targetDB.getCollection(sourceCollName).find({x: 1}).itcount(),
                "Target collection should contain the document after successful cross-DB rename",
            );

            // Clean up the target database via the same connection used for the rename.
            assert.commandWorked(targetDB.dropDatabase());
        }
    });

    it("Test that rename within the same database is not blocked when blockReplicaSetWrites is enabled", function () {
        const sourceCollName = "sameDbRenameSource";
        const targetCollName = "sameDbRenameTarget";

        // A rename within the same database does not insert data into the destination collection, so it
        // must succeed even while the write block is enabled. Issue it both through mongos and directly
        // against the shard's replica set primary, which differ only in the connection used.
        for (const {label, conn} of [
            {label: "mongos", conn: this.st.s},
            {label: "shard's replica set primary", conn: this.shard0Primary},
        ]) {
            const sourceColl = conn.getDB(this.testDBName).getCollection(sourceCollName);
            const targetColl = conn.getDB(this.testDBName).getCollection(targetCollName);
            assert.commandWorked(sourceColl.insert({x: 1}));

            // Enable write blocking on shard0 and check that the same-database rename still succeeds.
            enableReplicaSetWriteBlock(
                this.shard0PrimaryAdminDB,
                false /* allowDeletions */,
                "InsufficientDiskSpace" /* reason */,
            );
            assert.commandWorked(
                conn.adminCommand({
                    renameCollection: `${this.testDBName}.${sourceCollName}`,
                    to: `${this.testDBName}.${targetCollName}`,
                }),
                `Same-DB rename issued via ${label} should not be blocked`,
            );
            assert.eq(
                1,
                targetColl.find({x: 1}).itcount(),
                "Target collection should contain the document after successful same-DB rename",
            );

            // Disable write blocking and drop the target collection to reset state for the next connection.
            disableReplicaSetWriteBlock(
                this.shard0PrimaryAdminDB,
                "InsufficientDiskSpace" /* reason */,
            );
            assert(targetColl.drop());
        }
    });

    it("Test that new QE compact and cleanup are blocked when allowDeletions: false", function () {
        // blockedColl is a non-existent collection, so when the commands are not blocked we expect
        // them to fail with NamespaceNotFound. The deletions check fires before collection validation, so a
        // non-existent collection is sufficient to verify the rejection.
        const compactCmd = {compactStructuredEncryptionData: "blockedColl", compactionTokens: {}};
        const cleanupCmd = {cleanupStructuredEncryptionData: "blockedColl", cleanupTokens: {}};

        // Enable replica set write block with allowDeletions: false and check that new QE compact and cleanup are blocked.
        enableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );
        assert.commandFailedWithCode(
            this.testDB.runCommand(compactCmd),
            ErrorCodes.ReplicaSetWritesBlocked,
        );
        assert.commandFailedWithCode(
            this.testDB.runCommand(cleanupCmd),
            ErrorCodes.ReplicaSetWritesBlocked,
        );

        // Disable replica set write block and check that new QE compact and cleanup are not blocked anymore.
        disableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            "InsufficientDiskSpace" /* reason */,
        );
        assert.commandFailedWithCode(
            this.testDB.runCommand(compactCmd),
            ErrorCodes.NamespaceNotFound,
        );
        assert.commandFailedWithCode(
            this.testDB.runCommand(cleanupCmd),
            ErrorCodes.NamespaceNotFound,
        );
    });

    it("Test that new QE compact and cleanup complete end-to-end on a real encrypted collection when allowDeletions: true", function () {
        if (!isEnterpriseShell()) {
            jsTest.log.info("Skipping enterprise-only QE compact/cleanup write-block test");
            return;
        }
        const fleDbName = "block_replica_set_writes_fle";
        const fleCollName = "encrypted";
        const encryptedFields = {
            fields: [
                {
                    path: "first",
                    bsonType: "string",
                    queries: {queryType: "equality", contention: 0},
                },
            ],
        };
        runEncryptedTest(
            this.st.s.getDB("admin"),
            fleDbName,
            fleCollName,
            encryptedFields,
            (edb, client) => {
                const coll = edb[fleCollName];
                for (let i = 0; i < 5; i++) {
                    assert.commandWorked(coll.insert({first: "roger_" + i}));
                }
                try {
                    enableReplicaSetWriteBlock(
                        this.shard0PrimaryAdminDB,
                        true /* allowDeletions */,
                        "InsufficientDiskSpace",
                    );
                    // Check that compact and cleanup complete successfully when allowDeletions: true.
                    assert.commandWorked(coll.compact());
                    assert.commandWorked(coll.cleanup());
                } finally {
                    disableReplicaSetWriteBlock(this.shard0PrimaryAdminDB, "InsufficientDiskSpace");
                }
            },
        );
    });

    it("Test that a new incoming chunk migration is rejected when blockReplicaSetWrites is enabled", function () {
        // Shard the collection and move the chunk containing {x: 1} to shard1, so it can be
        // migrated back to shard0 (the to-be-blocked recipient).
        assert.commandWorked(this.testDB.createCollection(this.shardedCollName));
        assert.commandWorked(this.st.s.adminCommand({shardCollection: this.nns, key: {x: 1}}));
        assert.commandWorked(this.testShardedColl.insert({x: -1}));
        assert.commandWorked(this.testShardedColl.insert({x: 1}));
        assert.commandWorked(this.st.s.adminCommand({split: this.nns, middle: {x: 0}}));
        assert.commandWorked(
            this.st.s.adminCommand({
                moveChunk: this.nns,
                find: {x: 1},
                to: this.st.shard1.shardName,
                _waitForDelete: true,
            }),
        );

        // Enable replica set write blocking on shard0, the destination of the migration below.
        enableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );

        // A new incoming chunk migration to shard0 must be rejected while the block is enabled.
        assert.commandFailedWithCode(
            this.st.s.adminCommand({
                moveChunk: this.nns,
                find: {x: 1},
                to: this.st.shard0.shardName,
            }),
            ErrorCodes.ReplicaSetWritesBlocked,
        );
        assert.eq(
            1,
            this.st.shard1.getDB(this.testDBName)[this.shardedCollName].find({x: 1}).itcount(),
            "Expected {x: 1} to be on shard1 after failed migration following write block is enabled",
        );

        // Disable replica set write blocking and verify that incoming chunk migrations to shard0 succeed again.
        disableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            "InsufficientDiskSpace" /* reason */,
        );
        assert.commandWorked(
            this.st.s.adminCommand({
                moveChunk: this.nns,
                find: {x: 1},
                to: this.st.shard0.shardName,
                _waitForDelete: true,
            }),
        );
        assert.eq(
            1,
            this.st.shard0.getDB(this.testDBName)[this.shardedCollName].find({x: 1}).itcount(),
            "Expected {x: 1} to be on shard0 after migration following write block disable",
        );
    });

    it("Test that movePrimary is not allowed when moving data to a shard with blockReplicaSetWrites enabled", function () {
        // Create a separate database whose primary is shard1, then create multiple unsharded
        // collections in it so their data lives on shard1. Using more than one collection exercises
        // the recipient cleanup path, which must drop every partially-cloned collection.
        const movePrimaryDBName = `${this.testDBName}_movePrimary`;
        const collNames = ["unshardedColl1", "unshardedColl2"];
        this.extraDatabasesToDrop.push(movePrimaryDBName);
        assert.commandWorked(
            this.st.s.adminCommand({
                enablesharding: movePrimaryDBName,
                primaryShard: this.st.shard1.shardName,
            }),
        );
        const movePrimaryDB = this.st.s.getDB(movePrimaryDBName);
        for (const collName of collNames) {
            assert.commandWorked(movePrimaryDB[collName].insert([{x: 1}, {x: 2}]));
        }

        // Enable per-shard write blocking on shard0, the destination of the movePrimary below.
        enableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );

        const rejectedInsertsBefore = assert.commandWorked(this.shard0PrimaryAdminDB.serverStatus())
            .repl.replicaSetWritesBlockRejected.inserts;

        // movePrimary to shard0 must clone the collections onto shard0, whose writes are blocked, so
        // the cloner insert is rejected with ReplicaSetWritesBlocked and the operation fails.
        assert.commandFailedWithCode(
            this.st.s.adminCommand({movePrimary: movePrimaryDBName, to: this.st.shard0.shardName}),
            ErrorCodes.ReplicaSetWritesBlocked,
        );

        // The rejection must have gone through the replica set write block insert check.
        assert.gte(
            assert.commandWorked(this.shard0PrimaryAdminDB.serverStatus()).repl
                .replicaSetWritesBlockRejected.inserts,
            rejectedInsertsBefore + 1,
            "Aborted movePrimary should increment repl.replicaSetWritesBlockRejected.inserts",
        );

        // Check that the aborted operation leaves the cluster consistent
        assert.eq(
            this.st.shard1.shardName,
            this.st.config.databases.findOne({_id: movePrimaryDBName}).primary,
            "config.databases primary should still be the original donor after the aborted movePrimary",
        );
        for (const collName of collNames) {
            assert.eq(
                2,
                movePrimaryDB[collName].find().itcount(),
                "Collection should still be readable from the original primary after the aborted movePrimary",
            );
            assert(
                !this.st.shard0.getDB(movePrimaryDBName).getCollectionNames().includes(collName),
                "Recipient should not retain cloned data after the aborted movePrimary",
            );
        }

        // Disable per-shard write blocking and verify that movePrimary to shard0 now succeeds.
        disableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            "InsufficientDiskSpace" /* reason */,
        );
        assert.commandWorked(
            this.st.s.adminCommand({movePrimary: movePrimaryDBName, to: this.st.shard0.shardName}),
        );
        assert.eq(
            this.st.shard0.shardName,
            this.st.config.databases.findOne({_id: movePrimaryDBName}).primary,
            "config.databases primary should be the recipient after the successful movePrimary",
        );
        for (const collName of collNames) {
            assert.eq(
                2,
                this.st.shard0.getDB(movePrimaryDBName)[collName].find().itcount(),
                "Recipient should hold the cloned data after the successful movePrimary",
            );
        }
    });

    it("Test that movePrimary with only empty collections succeeds when the destination shard has blockReplicaSetWrites enabled", function () {
        // Create a separate database whose primary is shard1, then create multiple empty unsharded
        // collections in it. Cloning empty collections onto the destination bypasses the replica set
        // write block, so the movePrimary should succeed.
        const movePrimaryDBName = `${this.testDBName}_movePrimaryEmpty`;
        const collNames = ["emptyColl1", "emptyColl2"];
        this.extraDatabasesToDrop.push(movePrimaryDBName);
        assert.commandWorked(
            this.st.s.adminCommand({
                enablesharding: movePrimaryDBName,
                primaryShard: this.st.shard1.shardName,
            }),
        );
        const movePrimaryDB = this.st.s.getDB(movePrimaryDBName);
        for (const collName of collNames) {
            assert.commandWorked(movePrimaryDB.createCollection(collName));
        }

        // Enable per-shard write blocking on shard0, the destination of the movePrimary below.
        enableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );

        // movePrimary clones only empty collections onto shard0, which bypasses the write block, so
        // the operation succeeds even though writes are blocked on the destination.
        assert.commandWorked(
            this.st.s.adminCommand({movePrimary: movePrimaryDBName, to: this.st.shard0.shardName}),
        );

        // The primary should now be the recipient, and the (empty) collections should exist on it.
        assert.eq(
            this.st.shard0.shardName,
            this.st.config.databases.findOne({_id: movePrimaryDBName}).primary,
            "config.databases primary should be the recipient after the successful movePrimary",
        );
        for (const collName of collNames) {
            assert(
                this.st.shard0.getDB(movePrimaryDBName).getCollectionNames().includes(collName),
                "Recipient should hold the cloned empty collection after the successful movePrimary",
            );
            assert.eq(
                0,
                this.st.shard0.getDB(movePrimaryDBName)[collName].find().itcount(),
                "Cloned collection should be empty on the recipient",
            );
        }
    });

    it("Test that movePrimary aborts cleanly when blockReplicaSetWrites is enabled during cloning", function () {
        const movePrimaryDBName = `${this.testDBName}_movePrimaryMidClone`;
        this.extraDatabasesToDrop.push(movePrimaryDBName);
        assert.commandWorked(
            this.st.s.adminCommand({
                enablesharding: movePrimaryDBName,
                primaryShard: this.st.shard1.shardName,
            }),
        );
        const movePrimaryDB = this.st.s.getDB(movePrimaryDBName);
        assert.commandWorked(movePrimaryDB.unshardedColl.insert({x: 1}));

        const hangBeforeCloningData = configureFailPoint(
            this.st.rs1.getPrimary(),
            "hangBeforeCloningData",
        );

        const awaitMovePrimary = startParallelShell(
            funWithArgs(
                function (dbName, toShard) {
                    assert.commandFailedWithCode(
                        db.getSiblingDB("admin").runCommand({movePrimary: dbName, to: toShard}),
                        ErrorCodes.ReplicaSetWritesBlocked,
                    );
                },
                movePrimaryDBName,
                this.st.shard0.shardName,
            ),
            this.st.s.port,
        );

        try {
            hangBeforeCloningData.wait();

            // Enable per-shard write blocking on shard0 (the recipient) while the clone is paused, then
            // let the clone proceed: its first document insert must be rejected with ReplicaSetWritesBlocked.
            enableReplicaSetWriteBlock(
                this.shard0PrimaryAdminDB,
                false /* allowDeletions */,
                "InsufficientDiskSpace" /* reason */,
            );
        } finally {
            hangBeforeCloningData.off();
        }

        awaitMovePrimary();

        // Check that aborting the operation before committing leaves the cluster consistent.
        assert.eq(
            this.st.shard1.shardName,
            this.st.config.databases.findOne({_id: movePrimaryDBName}).primary,
            "config.databases primary should still be the donor after the mid-clone abort",
        );
        assert.eq(
            1,
            movePrimaryDB.unshardedColl.find({x: 1}).itcount(),
            "Collection should still be readable from the original primary after the mid-clone abort",
        );
        assert(
            !this.st.shard0.getDB(movePrimaryDBName).getCollectionNames().includes("unshardedColl"),
            "Recipient should not retain cloned data after the mid-clone abort",
        );
    });

    it("Test that movePrimary completes when blockReplicaSetWrites is enabled after cloning finishes", function () {
        const movePrimaryDBName = `${this.testDBName}_movePrimaryAfterClone`;
        this.extraDatabasesToDrop.push(movePrimaryDBName);
        assert.commandWorked(
            this.st.s.adminCommand({
                enablesharding: movePrimaryDBName,
                primaryShard: this.st.shard1.shardName,
            }),
        );
        const movePrimaryDB = this.st.s.getDB(movePrimaryDBName);
        assert.commandWorked(movePrimaryDB.unshardedColl.insert({x: 1}));

        const hangBeforeCriticalSection = configureFailPoint(
            this.st.rs1.getPrimary(),
            "hangBeforeMovePrimaryCriticalSection",
        );

        const awaitMovePrimary = startParallelShell(
            funWithArgs(
                function (dbName, toShard) {
                    assert.commandWorked(
                        db.getSiblingDB("admin").runCommand({movePrimary: dbName, to: toShard}),
                    );
                },
                movePrimaryDBName,
                this.st.shard0.shardName,
            ),
            this.st.s.port,
        );

        try {
            hangBeforeCriticalSection.wait();

            // Enable per-shard write blocking on shard0 (the recipient) after the clone finished.
            enableReplicaSetWriteBlock(
                this.shard0PrimaryAdminDB,
                false /* allowDeletions */,
                "InsufficientDiskSpace" /* reason */,
            );
        } finally {
            hangBeforeCriticalSection.off();
        }

        awaitMovePrimary();

        // Check that routing now points at the recipient and the data is readable there.
        assert.eq(
            this.st.shard0.shardName,
            this.st.config.databases.findOne({_id: movePrimaryDBName}).primary,
            "config.databases primary should be the recipient after the successful movePrimary",
        );
        assert.eq(
            1,
            movePrimaryDB.unshardedColl.find({x: 1}).itcount(),
            "Collection should be readable after movePrimary completes",
        );
    });

    it("Test that movePrimary off a shard with blockReplicaSetWrites enabled succeeds", function () {
        // The database primary is shard0, the to-be-blocked donor. Moving the primary off shard0
        // only clones data onto the recipient (shard1) and drops it on the donor, so blocking writes
        // on the donor must not prevent the operation: a full shard must still be able to shed data.
        const movePrimaryDBName = `${this.testDBName}_movePrimaryOffDonor`;
        this.extraDatabasesToDrop.push(movePrimaryDBName);
        assert.commandWorked(
            this.st.s.adminCommand({
                enablesharding: movePrimaryDBName,
                primaryShard: this.st.shard0.shardName,
            }),
        );
        const movePrimaryDB = this.st.s.getDB(movePrimaryDBName);
        assert.commandWorked(movePrimaryDB.unshardedColl.insert({x: 1}));

        enableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );

        assert.commandWorked(
            this.st.s.adminCommand({movePrimary: movePrimaryDBName, to: this.st.shard1.shardName}),
        );
        assert.eq(
            this.st.shard1.shardName,
            this.st.config.databases.findOne({_id: movePrimaryDBName}).primary,
            "config.databases primary should be the recipient after moving primary off the blocked donor",
        );
        assert.eq(
            1,
            movePrimaryDB.unshardedColl.find({x: 1}).itcount(),
            "Collection should be readable on the recipient after moving primary off the blocked donor",
        );
    });

    it("Test that an in-flight chunk migration completes when blockReplicaSetWrites is enabled mid-migration", function () {
        // Shard the collection and move the chunk containing {x: 1} to shard1, so it can be migrated
        // back to shard0 (the to-be-blocked recipient).
        assert.commandWorked(this.testDB.createCollection(this.shardedCollName));
        assert.commandWorked(this.st.s.adminCommand({shardCollection: this.nns, key: {x: 1}}));
        assert.commandWorked(this.testShardedColl.insert({x: -1}));
        assert.commandWorked(this.testShardedColl.insert({x: 1}));
        assert.commandWorked(this.st.s.adminCommand({split: this.nns, middle: {x: 0}}));
        assert.commandWorked(
            this.st.s.adminCommand({
                moveChunk: this.nns,
                find: {x: 1},
                to: this.st.shard1.shardName,
                _waitForDelete: true,
            }),
        );

        // Pause the recipient (shard0) migrate thread after the collection has been created but
        // before the bulk clone, i.e. once the incoming-migration start gate has already been
        // passed. This is the window in which the cloning bypass (rather than the start gate) governs
        // whether the migration is allowed to write.
        const hangRecipientBeforeClone = configureFailPoint(
            this.st.rs0.getPrimary(),
            "migrateThreadHangAtStep3",
        );

        const awaitMoveChunk = startParallelShell(
            funWithArgs(
                function (ns, toShard) {
                    assert.commandWorked(
                        db.getSiblingDB("admin").runCommand({
                            moveChunk: ns,
                            find: {x: 1},
                            to: toShard,
                        }),
                    );
                },
                this.nns,
                this.st.shard0.shardName,
            ),
            this.st.s.port,
        );

        try {
            hangRecipientBeforeClone.wait();

            // Enable per-shard write blocking on shard0 while the migration is in flight. The cloning
            // inserts enable ReplicaSetWriteBlockBypass, so the migration must still complete rather than
            // abort.
            enableReplicaSetWriteBlock(
                this.shard0PrimaryAdminDB,
                false /* allowDeletions */,
                "InsufficientDiskSpace" /* reason */,
            );
        } finally {
            hangRecipientBeforeClone.off();
        }

        awaitMoveChunk();

        assert.eq(
            1,
            this.st.shard0.getDB(this.testDBName)[this.shardedCollName].find({x: 1}).itcount(),
            "Migrated document should be on shard0 after the in-flight migration completes under the block",
        );
    });

    it("Test that an in-flight chunk migration completes when blockReplicaSetWrites is enabled during the batch apply (catchup) phase", function () {
        // Shard the collection and move the chunk containing {x: 1} to shard1, so it can be migrated
        // back to shard0 (the to-be-blocked recipient).
        assert.commandWorked(this.testDB.createCollection(this.shardedCollName));
        assert.commandWorked(this.st.s.adminCommand({shardCollection: this.nns, key: {x: 1}}));
        assert.commandWorked(this.testShardedColl.insert({x: -1}));
        assert.commandWorked(this.testShardedColl.insert({x: 1}));
        assert.commandWorked(this.st.s.adminCommand({split: this.nns, middle: {x: 0}}));
        assert.commandWorked(
            this.st.s.adminCommand({
                moveChunk: this.nns,
                find: {x: 1},
                to: this.st.shard1.shardName,
                _waitForDelete: true,
            }),
        );

        // Pause the recipient (shard0) migrate thread after the initial bulk clone has finished but
        // before the catchup phase, where transferred modifications are applied via the batch
        // applier. This is the window that exercises the batch-apply write path, which runs on a
        // separate operation context that must carry the ReplicaSetWriteBlockBypass.
        const hangRecipientAfterClone = configureFailPoint(
            this.st.rs0.getPrimary(),
            "migrateThreadHangAtStep4",
        );

        const awaitMoveChunk = startParallelShell(
            funWithArgs(
                function (ns, toShard) {
                    assert.commandWorked(
                        db.getSiblingDB("admin").runCommand({
                            moveChunk: ns,
                            find: {x: 1},
                            to: toShard,
                        }),
                    );
                },
                this.nns,
                this.st.shard0.shardName,
            ),
            this.st.s.port,
        );

        try {
            hangRecipientAfterClone.wait();

            // Insert an additional document into the migrating chunk on the donor. Because the bulk
            // clone has already finished, this write is logged as a modification that must be
            // transferred to and applied by the recipient during the catchup (batch apply) phase.
            assert.commandWorked(this.testShardedColl.insert({x: 2}));

            // Enable per-shard write blocking on shard0 while the migration is in flight. The batch
            // applier that applies the transferred modification enables ReplicaSetWriteBlockBypass,
            // so the migration must still complete rather than abort.
            enableReplicaSetWriteBlock(
                this.shard0PrimaryAdminDB,
                false /* allowDeletions */,
                "InsufficientDiskSpace" /* reason */,
            );
        } finally {
            hangRecipientAfterClone.off();
        }

        awaitMoveChunk();

        // Both the cloned document and the one applied during the catchup phase must be on shard0.
        assert.eq(
            2,
            this.st.shard0
                .getDB(this.testDBName)
                [this.shardedCollName].find({x: {$gte: 0}})
                .itcount(),
            "Both migrated documents should be on shard0 after the migration completes under the block",
        );
    });

    it("Test that in-progress resharding is held (coordinator and recipient remain non-terminal and the on-hold warning is logged) and resumes when blockReplicaSetWrites is enabled then disabled on a recipient shard", function () {
        // Shard the collection on {x: 1}. All data initially lives on shard0 (primary shard).
        assert.commandWorked(this.testDB.createCollection(this.shardedCollName));
        assert.commandWorked(this.st.s.adminCommand({shardCollection: this.nns, key: {x: 1}}));
        assert.commandWorked(this.testShardedColl.insert({x: 1, y: 1}));
        assert.commandWorked(this.testShardedColl.insert({x: 2, y: 2}));

        // Pause the resharding recipient on shard0 before cloning begins, so we can enable the
        // per-shard write block while a resharding operation is in progress.
        const hangFp = configureFailPoint(
            this.shard0Primary,
            "reshardingPauseRecipientBeforeCloning",
        );

        // Start a resharding in a parallel shell. ReplicaSetWritesBlocked is now a retryable error,
        // so the recipient pauses on its blocked clone writes: the operation is
        // held and is expected to eventually succeed once the block is lifted.
        const awaitResharding = startParallelShell(
            funWithArgs((ns) => {
                assert.commandWorked(
                    db.adminCommand({reshardCollection: ns, key: {y: 1}, numInitialChunks: 2}),
                );
            }, this.nns),
            this.st.s.port,
        );

        // Wait until shard0's resharding recipient has reached the pre-cloning pause.
        hangFp.wait();

        // Enable per-shard write blocking on shard0, then let the recipient proceed into cloning.
        enableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );

        hangFp.off();

        // The recipient's clone writes to the temporary resharding collection are now blocked. It
        // should retry/hold and log the "on hold" warning.
        checkLog.containsJson(this.shard0Primary, 12818901);

        // The coordinator is paused indirectly: it is waiting on the held recipient, so its state
        // doc stays in a pre-commit state (it has not persisted a commit decision).
        const coordinatorDoc = this.st.s
            .getCollection("config.reshardingOperations")
            .findOne({ns: this.nns});
        assert.neq(null, coordinatorDoc, "expected an in-progress resharding coordinator document");
        assert(
            coordinatorDoc.state !== "decision-persisted" && coordinatorDoc.state !== "done",
            "expected resharding to be held in a pre-commit state while writes are blocked",
            {state: coordinatorDoc.state},
        );

        // The recipient is held, not aborted: its local state document still exists and is in a
        // non-terminal state (not kError/kDone).
        const recipientDoc = this.shard0Primary
            .getDB("config")
            .getCollection("localReshardingOperations.recipient")
            .findOne({});
        assert.neq(
            null,
            recipientDoc,
            "expected the recipient state document to still exist while held (not aborted)",
        );
        assert(
            recipientDoc.mutableState.state !== "error" &&
                recipientDoc.mutableState.state !== "done",
            "expected the held recipient to remain in a non-terminal state",
            {state: recipientDoc.mutableState.state},
        );

        // Disable write blocking and verify that the held resharding now resumes and completes.
        disableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            "InsufficientDiskSpace" /* reason */,
        );
        awaitResharding();
        assert.eq(
            2,
            this.testShardedColl.find().itcount(),
            "Expected two documents after the held resharding resumed and completed",
        );
    });

    it("Test that an in-progress resharding held by blockReplicaSetWrites can be manually aborted", function () {
        // Shard the collection on {x: 1}. All data initially lives on shard0 (primary shard).
        assert.commandWorked(this.testDB.createCollection(this.shardedCollName));
        assert.commandWorked(this.st.s.adminCommand({shardCollection: this.nns, key: {x: 1}}));
        assert.commandWorked(this.testShardedColl.insert({x: 1, y: 1}));
        assert.commandWorked(this.testShardedColl.insert({x: 2, y: 2}));

        // Pause the resharding recipient on shard0 before cloning begins, so we can enable the
        // per-shard write block while a resharding operation is in progress.
        const hangFp = configureFailPoint(
            this.shard0Primary,
            "reshardingPauseRecipientBeforeCloning",
        );

        // Start a resharding in a parallel shell. Once the recipient is held on its blocked clone
        // writes, we manually abort the operation, so the original command is expected to fail with
        // ReshardCollectionAborted.
        const awaitResharding = startParallelShell(
            funWithArgs((ns) => {
                assert.commandFailedWithCode(
                    db.adminCommand({reshardCollection: ns, key: {y: 1}, numInitialChunks: 2}),
                    ErrorCodes.ReshardCollectionAborted,
                );
            }, this.nns),
            this.st.s.port,
        );

        // Wait until shard0's resharding recipient has reached the pre-cloning pause.
        hangFp.wait();

        // Enable per-shard write blocking on shard0, then let the recipient proceed into cloning.
        enableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );

        hangFp.off();

        // The recipient's clone writes to the temporary resharding collection are now blocked, so
        // it holds and logs the "on hold" warning.
        checkLog.containsJson(this.shard0Primary, 12818901);

        // Manually abort the held resharding operation while the write block is still enabled. The
        // abort cancels the recipient's retry loop, so the operation winds down without needing the
        // block to be lifted first.
        assert.commandWorked(this.st.s.adminCommand({abortReshardCollection: this.nns}));

        // The original reshardCollection command should observe the abort.
        awaitResharding();

        // The coordinator document should be cleaned up, and the source collection left unchanged.
        assert.eq(
            null,
            this.st.s.getCollection("config.reshardingOperations").findOne({ns: this.nns}),
            "Expected no in-progress resharding coordinator document after manual abort",
        );
        assert.eq(
            2,
            this.testShardedColl.find().itcount(),
            "Expected the source collection to be unchanged after the held resharding was aborted",
        );
    });

    it("Test that a new resharding fails when started while blockReplicaSetWrites is enabled on a recipient shard", function () {
        // Shard the collection on {x: 1}. All data initially lives on shard0 (primary shard).
        assert.commandWorked(this.testDB.createCollection(this.shardedCollName));
        assert.commandWorked(this.st.s.adminCommand({shardCollection: this.nns, key: {x: 1}}));
        assert.commandWorked(this.testShardedColl.insert({x: 1, y: 1}));
        assert.commandWorked(this.testShardedColl.insert({x: 2, y: 2}));

        // Enable per-shard write blocking on shard0 before starting resharding.
        enableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );

        // The recipient start gate rejects incoming resharding while writes are blocked, which
        // causes the coordinator to abort the operation.
        const awaitResharding = startParallelShell(
            funWithArgs((ns) => {
                assert.commandFailedWithCode(
                    db.adminCommand({reshardCollection: ns, key: {y: 1}, numInitialChunks: 2}),
                    ErrorCodes.ReplicaSetWritesBlocked,
                );
            }, this.nns),
            this.st.s.port,
        );
        awaitResharding();

        assert.eq(
            2,
            this.testShardedColl.find().itcount(),
            "Expected collection to be unchanged after aborted resharding",
        );
        assert.eq(
            null,
            this.st.s.getCollection("config.reshardingOperations").findOne({ns: this.nns}),
            "Expected no in-progress resharding coordinator document after abort",
        );
    });

    it("Test that a dbPrimary-only recipient (receiving only metadata and not data) does not cause resharding to abort when blockReplicaSetWrites is already enabled", function () {
        // shard0 is the db primary for this.testDBName. Moving an unsharded collection to shard1
        // makes shard0 a dbPrimary-only resharding recipient. blockReplicaSetWrites
        // should therefore skip aborting it, allowing the moveCollection to complete.
        const unshardedCollName = "dbPrimaryOnlyRecipientColl";
        const ns = `${this.testDBName}.${unshardedCollName}`;
        const testColl = this.testDB[unshardedCollName];

        assert.commandWorked(testColl.insert({x: 1}));
        assert.commandWorked(testColl.insert({x: 2}));

        // Pause shard1's resharding recipient before it begins cloning so write blocking can be
        // enabled while the operation is in progress.
        const shard1Primary = this.st.rs1.getPrimary();
        const hangFp = configureFailPoint(shard1Primary, "reshardingPauseRecipientBeforeCloning");

        // Move the collection from shard0 to shard1.
        const shard1ShardName = this.st.shard1.shardName;
        const awaitMoveCollection = startParallelShell(
            funWithArgs(
                (ns, toShard) => {
                    assert.commandWorked(db.adminCommand({moveCollection: ns, toShard}));
                },
                ns,
                shard1ShardName,
            ),
            this.st.s.port,
        );

        hangFp.wait();

        // Enable per-shard write blocking.
        enableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );

        hangFp.off();

        // Check that moveCollection succeeds.
        awaitMoveCollection();

        assert.eq(2, testColl.find().itcount(), "Expected two documents after moveCollection", {
            ns,
        });
    });

    it("Test that a dbPrimary-only recipient (receiving only metadata and not data) can start while blockReplicaSetWrites is already enabled", function () {
        const unshardedCollName = "dbPrimaryOnlyRecipientBlockFirstColl";
        const ns = `${this.testDBName}.${unshardedCollName}`;
        const testColl = this.testDB[unshardedCollName];
        assert.commandWorked(testColl.insert({x: 1}));
        assert.commandWorked(testColl.insert({x: 2}));

        // Enable per-shard write blocking on shard0 and check that the resharding operation succeeds.
        enableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );
        assert.commandWorked(
            this.st.s.adminCommand({moveCollection: ns, toShard: this.st.shard1.shardName}),
        );

        assert.eq(2, testColl.find().itcount(), "Expected two documents after moveCollection", {
            ns,
        });
    });

    it("Test that an in-progress resharding is held during the index building phase when blockReplicaSetWrites is enabled, and resumes when the block is lifted", function () {
        // Shard the collection on {x: 1}. All data initially lives on shard0 (primary shard).
        assert.commandWorked(this.testDB.createCollection(this.shardedCollName));
        assert.commandWorked(this.st.s.adminCommand({shardCollection: this.nns, key: {x: 1}}));
        assert.commandWorked(this.testShardedColl.insert({x: 1, y: 1}));
        assert.commandWorked(this.testShardedColl.insert({x: 2, y: 2}));

        // Hold the resharding index build in-flight.
        const hangFp = configureFailPoint(this.shard0Primary, "hangAfterInitializingIndexBuild");

        // Start resharding in a parallel shell.
        const awaitResharding = startParallelShell(
            funWithArgs((ns) => {
                assert.commandWorked(
                    db.adminCommand({reshardCollection: ns, key: {y: 1}, numInitialChunks: 2}),
                );
            }, this.nns),
            this.st.s.port,
        );

        // Wait until the resharding index build is paused mid-build.
        hangFp.wait();

        // Enable per-shard write blocking on shard0 which aborts the in-flight index build.
        enableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );
        hangFp.off();

        // The recipient retries the index-build phase, but the block is still active, so the
        // restarted build is now rejected up front with ReplicaSetWritesBlocked and the recipient
        // holds, logging the warning.
        checkLog.containsJson(this.shard0Primary, 12818900);

        // The coordinator must be in a non-terminal, pre-commit state while the recipient is held.
        const coordinatorDoc = this.st.s
            .getCollection("config.reshardingOperations")
            .findOne({ns: this.nns});
        assert.neq(null, coordinatorDoc, "expected an in-progress resharding coordinator document");
        assert(
            coordinatorDoc.state !== "decision-persisted" && coordinatorDoc.state !== "done",
            "expected resharding to be held in a pre-commit state while writes are blocked",
            {state: coordinatorDoc.state},
        );

        // The recipient must still exist in a non-terminal state.
        const recipientDoc = this.shard0Primary
            .getDB("config")
            .getCollection("localReshardingOperations.recipient")
            .findOne({});
        assert.neq(null, recipientDoc, "expected recipient state document to exist while held");
        assert(
            recipientDoc.mutableState.state !== "error" &&
                recipientDoc.mutableState.state !== "done",
            "expected the held recipient to remain in a non-terminal state",
            {state: recipientDoc.mutableState.state},
        );

        // Lift the write block. The recipient resumes building indexes and the operation completes.
        disableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            "InsufficientDiskSpace" /* reason */,
        );
        awaitResharding();
        assert.eq(
            2,
            this.testShardedColl.find().itcount(),
            "Expected two documents after the held resharding resumed and completed",
        );
    });

    it("Test that in-progress resharding is held during the kApplying oplog application phase when blockReplicaSetWrites is enabled, and resumes when the block is lifted", function () {
        // Shard the collection on {x: 1}. All data initially lives on shard0 (primary shard),
        // which will remain the sole donor. shard1 owns none of the pre-resharding data.
        assert.commandWorked(this.testDB.createCollection(this.shardedCollName));
        assert.commandWorked(this.st.s.adminCommand({shardCollection: this.nns, key: {x: 1}}));
        assert.commandWorked(this.testShardedColl.insert({x: 1, y: 1}));
        assert.commandWorked(this.testShardedColl.insert({x: 2, y: 2}));

        // Pin the entire new {y: 1} range to shard1 so it is a pure recipient (never a donor).
        // This matters because once the coordinator reaches kBlockingWrites, resharding
        // permanently bypasses the write block on the recipient's oplog applier so the final
        // catch-up cannot be blocked.
        const shard1Primary = this.st.rs1.getPrimary();
        const shard1PrimaryAdminDB = shard1Primary.getDB("admin");
        const shard1Name = this.st.shard1.shardName;

        // Hold the coordinator's commit monitor so it can't reach "blocking-writes", which is what
        // grants the applier's write-block bypass.
        const configPrimary = this.st.configRS.getPrimary();
        const coordFp = configureFailPoint(configPrimary, "hangBeforeQueryingRecipients");

        // Pause shard1's recipient just before it starts oplog application (kApplying phase),
        // so we can enable the write block before the appliers begin writing.
        const hangFp = configureFailPoint(
            shard1Primary,
            "reshardingPauseRecipientBeforeOplogApplication",
        );

        // Start resharding in a parallel shell.
        const awaitResharding = startParallelShell(
            funWithArgs(
                (ns, shard1Name) => {
                    assert.commandWorked(
                        db.adminCommand({
                            reshardCollection: ns,
                            key: {y: 1},
                            shardDistribution: [
                                {shard: shard1Name, min: {y: MinKey}, max: {y: MaxKey}},
                            ],
                        }),
                    );
                },
                this.nns,
                shard1Name,
            ),
            this.st.s.port,
        );

        // Wait until shard1's recipient has reached the pre-oplog-application pause. While the
        // recipient is held here it has not yet reported the "applying" state to the coordinator
        // (that update is sent only after this failpoint is released), so the coordinator stays in
        // the "cloning" phase.
        hangFp.wait();

        // Write during the resharding window, after the recipient has cloned, so that the donor
        // produces an oplog entry for the recipient to apply once oplog application begins. This write must happen before the block is enabled.
        assert.commandWorked(this.testShardedColl.insert({x: 3, y: 3}));

        // Enable per-shard write blocking on shard1, then release the failpoint
        // so the recipient starts oplog application with the block already active.
        enableReplicaSetWriteBlock(
            shard1PrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );
        hangFp.off();

        // The oplog applier hits ReplicaSetWritesBlocked and holds, logging the on-hold warning.
        checkLog.containsJson(shard1Primary, 12818902);

        // The coordinator must remain in a non-terminal, pre-commit state while the recipient
        // applier is held.
        const coordinatorDoc = this.st.s
            .getCollection("config.reshardingOperations")
            .findOne({ns: this.nns});
        assert.neq(null, coordinatorDoc, "expected an in-progress resharding coordinator document");
        assert(
            coordinatorDoc.state !== "decision-persisted" && coordinatorDoc.state !== "done",
            "expected resharding to be held in a pre-commit state while oplog application is blocked",
            {state: coordinatorDoc.state},
        );

        // The recipient must still be alive in a non-terminal state.
        const recipientDoc = shard1Primary
            .getDB("config")
            .getCollection("localReshardingOperations.recipient")
            .findOne({});
        assert.neq(null, recipientDoc, "expected recipient state document to exist while held");
        assert(
            recipientDoc.mutableState.state !== "error" &&
                recipientDoc.mutableState.state !== "done",
            "expected the held recipient to remain in a non-terminal state",
            {state: recipientDoc.mutableState.state},
        );

        // Let the coordinator resume: it queries the recipients, advances to "blocking-writes",
        // and enables the write-block bypass on the appliers. Then lift the write block. The
        // oplog applier retries, drains the remaining entries, and the resharding operation
        // completes.
        coordFp.off();
        disableReplicaSetWriteBlock(shard1PrimaryAdminDB, "InsufficientDiskSpace" /* reason */);
        awaitResharding();
        assert.eq(
            3,
            this.testShardedColl.find().itcount(),
            "Expected three documents after the held resharding resumed and completed",
        );
    });

    it("Test that resharding completes through the kStrictConsistency catch-up phase even when blockReplicaSetWrites is enabled after donors have blocked writes", function () {
        // Shard the collection and place both documents on shard0 (the primary shard).
        assert.commandWorked(this.testDB.createCollection(this.shardedCollName));
        assert.commandWorked(this.st.s.adminCommand({shardCollection: this.nns, key: {x: 1}}));
        assert.commandWorked(this.testShardedColl.insert({x: 1, y: 1}));
        assert.commandWorked(this.testShardedColl.insert({x: 2, y: 2}));

        // Pause shard0's recipient just before it calls awaitStrictlyConsistent(). The oplog
        // appliers continue running on separate threads, so the coordinator can still observe
        // low oplog lag and advance to kBlockingWrites while the state-machine thread is paused.
        const hangFp = configureFailPoint(
            this.shard0Primary,
            "reshardingPauseRecipientDuringOplogApplication",
        );

        // Start resharding in a parallel shell, expecting it to complete despite the write block
        // that will be enabled mid-operation.
        const awaitResharding = startParallelShell(
            funWithArgs((ns) => {
                assert.commandWorked(
                    db.adminCommand({reshardCollection: ns, key: {y: 1}, numInitialChunks: 2}),
                );
            }, this.nns),
            this.st.s.port,
        );

        // Wait until shard0's recipient has reached the pre-strict-consistency pause.
        hangFp.wait();

        // Poll until the coordinator has advanced to "blocking-writes". At that point the
        // coordinator has sent kBlockingWrites to all donors, which has triggered
        // prepareForCriticalSection() on the recipient and enabled the write-block bypass on
        // all oplog appliers.
        assert.soon(
            () => {
                const doc = this.st.s
                    .getCollection("config.reshardingOperations")
                    .findOne({ns: this.nns});
                return doc && doc.state === "blocking-writes";
            },
            "Coordinator did not reach blocking-writes state while recipient was paused",
            2 * 60 * 1000,
            200,
        );

        // Enable the per-shard write block.
        enableReplicaSetWriteBlock(
            this.shard0PrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );

        // Release the recipient. The oplog drain completes via the write-block bypass, and the
        // resharding operation terminates successfully without ever disabling the write block.
        hangFp.off();

        awaitResharding();
        assert.eq(
            2,
            this.testShardedColl.find().itcount(),
            "Expected two documents after resharding completed through kStrictConsistency with write block active",
        );
    });

    it("Test that blockReplicaSetWrites does not abort a resharding recipient that has already received a coordinator commit decision", function () {
        // Shard on {x: 1} and place the two documents on different shards, so that the subsequent
        // resharding forces shard0 to clone {y: 2} from shard1. That makes shard0 a genuine,
        // non-dbPrimary-only recipient, so the only reason blockReplicaSetWrites can have to skip
        // aborting it is that it has already received the coordinator's commit decision.
        assert.commandWorked(this.testDB.createCollection(this.shardedCollName));
        assert.commandWorked(this.st.s.adminCommand({shardCollection: this.nns, key: {x: 1}}));
        assert.commandWorked(this.testShardedColl.insert({x: 1, y: 1}));
        assert.commandWorked(this.testShardedColl.insert({x: 2, y: 2}));
        assert.commandWorked(this.st.s.adminCommand({split: this.nns, middle: {x: 2}}));
        assert.commandWorked(
            this.st.s.adminCommand({
                moveChunk: this.nns,
                find: {x: 2},
                to: this.st.shard1.shardName,
            }),
        );

        // Pause shard0's recipient just before collection cleanup. At this point the coordinator
        // has already persisted its commit decision.
        const hangFp = configureFailPoint(
            this.shard0Primary,
            "reshardingPauseRecipientBeforeCleanup",
        );

        // Reshard onto {y: 1} with all output owned by shard0, forcing it to clone {y: 2} from
        // shard1.
        const shard0Name = this.st.shard0.shardName;
        const awaitResharding = startParallelShell(
            funWithArgs(
                (ns, shard0Name) => {
                    assert.commandWorked(
                        db.adminCommand({
                            reshardCollection: ns,
                            key: {y: 1},
                            shardDistribution: [
                                {shard: shard0Name, min: {y: MinKey}, max: {y: MaxKey}},
                            ],
                        }),
                    );
                },
                this.nns,
                shard0Name,
            ),
            this.st.s.port,
        );

        // Wait until shard0's recipient has reached the post-commit cleanup pause, confirming the
        // coordinator commit decision has been persisted and the collection rename is done.
        hangFp.wait();

        // Enable per-shard write blocking on shard0 from a parallel shell so the command can run
        // concurrently with the paused recipient.
        const awaitWriteBlock = startParallelShell(() => {
            assert.commandWorked(
                db.adminCommand({
                    blockReplicaSetWrites: 1,
                    enabled: true,
                    allowDeletions: false,
                    reason: "InsufficientDiskSpace",
                }),
            );
        }, this.shard0Primary.port);

        // Release the recipient so it can finish its cleanup.
        hangFp.off();

        // Check that the resharding succeeds and the write block is established.
        awaitResharding();
        awaitWriteBlock();
        assert.eq(
            2,
            this.testShardedColl.find().itcount(),
            "Expected two documents after resharding",
        );
    });
});
