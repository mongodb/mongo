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
 *   assumes_read_concern_unchanged,
 *   assumes_read_preference_unchanged,
 *   featureFlagBlockReplicaSetWrites,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {checkLog} from "src/mongo/shell/check_log.js";

function disableReplicaSetWriteBlock(adminDB) {
    // Wait for replica set write block to be disabled. This is necessarry because the command can fail
    // transiently with a lock contantion with previous operations. For example, when the range deleter
    // background thread becomes active, it hits ReplicaSetWritesBlocked error and retries in a tight loop
    // without releasing its collection lock, so when disableReplicaSetWriteBlock is called immediately after,
    // it can transiently fail due to lock contention.
    assert.soon(() => {
        const res = adminDB.runCommand({
            blockReplicaSetWrites: 1,
            enabled: false,
            reason: "InsufficientDiskSpace",
        });
        return res.ok;
    }, "Failed to disable replica set write block");
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

        assert.commandWorked(
            this.st.s.adminCommand({enablesharding: this.testDBName, primaryShard: this.st.shard0.shardName}),
        );
        this.testDB = this.st.s.getDB(this.testDBName);
        this.testShardedColl = this.testDB.getCollection(this.shardedCollName);
    });

    afterEach(function () {
        // Disable replica set and global user write block (if not previously enabled the operation is a no-op).
        disableReplicaSetWriteBlock(this.shard0PrimaryAdminDB);
        assert.commandWorked(
            this.st.s.adminCommand({
                setUserWriteBlockMode: 1,
                global: false,
                reason: "DiskUseThresholdExceeded",
            }),
        );

        // Drop test database.
        assert.commandWorked(this.testDB.dropDatabase());
    });

    after(function () {
        this.st.stop();
    });

    it("Test blockReplicaSetWrites command counter increments on enable", function () {
        // Enable per-shard write blocking on shard0.
        assert.commandWorked(
            this.shard0PrimaryAdminDB.runCommand({
                blockReplicaSetWrites: 1,
                enabled: true,
                allowDeletions: false,
                reason: "InsufficientDiskSpace",
            }),
        );

        // Check replica set write block, reason, and command counter metrics when blockReplicaSetWrites is enabled.
        let replStatus = assert.commandWorked(this.shard0PrimaryAdminDB.serverStatus()).repl;
        assert.eq(replStatus.replicaSetWriteBlock, 2, "replicaSetWriteBlock metric should be 2 (Enabled)");
        assert.eq(
            replStatus.replicaSetWriteBlockReason,
            0,
            "replicaSetWriteBlockReason metric should be 0 (InsufficientDiskSpace)",
        );
        assert.eq(
            replStatus.replicaSetWritesBlockCounters.InsufficientDiskSpace,
            1,
            "repl.replicaSetWritesBlockCounters counter for InsufficientDiskSpace should be 1",
        );

        //Disable per-shard write blocking on shard0.
        disableReplicaSetWriteBlock(this.shard0PrimaryAdminDB);

        // Check replica set write block, reason, and command counter when blockReplicaSetWrites is disabled.
        replStatus = assert.commandWorked(this.shard0PrimaryAdminDB.serverStatus()).repl;
        assert.eq(replStatus.replicaSetWriteBlock, 1, "replicaSetWriteBlock metric should be 1 (Disabled)");
        assert(
            !replStatus.hasOwnProperty("replicaSetWriteBlockReason"),
            "replicaSetWriteBlockReason should be absent when replica set write block is disabled",
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
        assert.commandWorked(
            this.shard0PrimaryAdminDB.runCommand({
                blockReplicaSetWrites: 1,
                enabled: true,
                allowDeletions: false,
                reason: "InsufficientDiskSpace",
            }),
        );

        // Check blocked inserts, updates, and deletes are blocked and counters are incremented.
        let replStatus = assert.commandWorked(this.shard0PrimaryAdminDB.serverStatus()).repl;
        assert(replStatus.hasOwnProperty("replicaSetWritesBlockRejected"));
        const insertsAfterEnable = replStatus.replicaSetWritesBlockRejected.inserts;
        const updatesAfterEnable = replStatus.replicaSetWritesBlockRejected.updates;
        const deletesAfterEnable = replStatus.replicaSetWritesBlockRejected.deletes;

        assert.commandFailedWithCode(this.testShardedColl.insert({x: 1}), ErrorCodes.ReplicaSetWritesBlocked);
        assert.commandFailedWithCode(this.testShardedColl.insert({x: 2}), ErrorCodes.ReplicaSetWritesBlocked);
        assert.commandFailedWithCode(
            this.testShardedColl.update({x: 0}, {$set: {y: 1}}),
            ErrorCodes.ReplicaSetWritesBlocked,
        );
        assert.commandFailedWithCode(this.testShardedColl.remove({x: 0}), ErrorCodes.ReplicaSetWritesBlocked);

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
        assert.commandFailedWithCode(this.testShardedColl.insert({x: 2}), ErrorCodes.UserWritesBlocked);

        // Enable per-shard write blocking on shard0 and check that insert still fails with
        // UserWritesBlocked error.
        assert.commandWorked(
            this.shard0PrimaryAdminDB.runCommand({
                blockReplicaSetWrites: 1,
                enabled: true,
                allowDeletions: false,
                reason: "InsufficientDiskSpace",
            }),
        );
        assert.commandFailedWithCode(this.testShardedColl.insert({x: 3}), ErrorCodes.UserWritesBlocked);

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

        // Disable per-shard write blocking on shard0 and check that insert succeeds.
        disableReplicaSetWriteBlock(this.shard0PrimaryAdminDB);
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
        assert.commandWorked(this.st.s.adminCommand({moveChunk: this.nns, find: {x: 0}, to: this.st.shard1.shardName}));

        // Drop mongos routing cache so that it is forced to load the updated catalog metadata, which
        // guarantees operations are routed to the correct shard.
        assert.commandWorked(this.st.s.adminCommand({flushRouterConfig: this.testDBName}));

        // Enable per-shard write blocking on shard0.
        assert.commandWorked(
            this.shard0PrimaryAdminDB.runCommand({
                blockReplicaSetWrites: 1,
                enabled: true,
                allowDeletions: false,
                reason: "InsufficientDiskSpace",
            }),
        );

        // Check that insert, update, and delete operations routed to shard0 fail with ReplicaSetWritesBlocked error.
        assert.commandFailedWithCode(this.testShardedColl.insert({x: -2, y: "c"}), ErrorCodes.ReplicaSetWritesBlocked);
        assert.commandFailedWithCode(
            this.testShardedColl.update({x: -1}, {$set: {y: "d"}}),
            ErrorCodes.ReplicaSetWritesBlocked,
        );
        assert.commandFailedWithCode(this.testShardedColl.remove({x: -1}), ErrorCodes.ReplicaSetWritesBlocked);
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
        assert.commandWorked(
            this.shard0PrimaryAdminDB.runCommand({
                blockReplicaSetWrites: 1,
                enabled: true,
                allowDeletions: false,
                reason: "InsufficientDiskSpace",
            }),
        );
        assert.commandFailedWithCode(testColl.remove({_id: 1}), ErrorCodes.ReplicaSetWritesBlocked);
        assert.eq(2, testColl.count(), "Both documents should remain while deletions are blocked");

        // Disable write block and re-enable with allowDeletions: true — user deletes should succeed.
        disableReplicaSetWriteBlock(this.shard0PrimaryAdminDB);

        assert.commandWorked(
            this.shard0PrimaryAdminDB.runCommand({
                blockReplicaSetWrites: 1,
                enabled: true,
                allowDeletions: true,
                reason: "InsufficientDiskSpace",
            }),
        );
        assert.commandWorked(testColl.remove({_id: 1}));
        assert.eq(1, testColl.count(), "Document should have been deleted when allowDeletions is true");

        // Inserts and updates should still be blocked when allowDeletions is true.
        assert.commandFailedWithCode(testColl.insert({_id: 3}), ErrorCodes.ReplicaSetWritesBlocked);
        assert.commandFailedWithCode(testColl.update({_id: 2}, {$set: {x: 1}}), ErrorCodes.ReplicaSetWritesBlocked);
    });

    it("Test range deletion on donor is blocked when allowDeletions is false and allowed when allowDeletions is true", function () {
        const shard0LocalColl = this.st.shard0.getDB(this.testDBName).getCollection(this.shardedCollName);

        // Shard the collection and insert some documents.
        assert.commandWorked(this.testDB.createCollection(this.shardedCollName));
        assert.commandWorked(this.st.s.adminCommand({shardCollection: this.nns, key: {x: 1}}));
        assert.commandWorked(this.testShardedColl.insert({x: -1}));
        assert.commandWorked(this.testShardedColl.insert({x: 1}));

        // Suspend range deletion on shard0.
        const suspendRangeDel = configureFailPoint(this.st.shard0, "suspendRangeDeletion");

        // Split the chunk and move it to shard1.
        assert.commandWorked(this.st.s.adminCommand({split: this.nns, middle: {x: 0}}));
        assert.commandWorked(this.st.s.adminCommand({moveChunk: this.nns, find: {x: 0}, to: this.st.shard1.shardName}));

        // Check that the orphaned document is still present on shard0.
        assert.eq(
            1,
            shard0LocalColl.find({x: 1}).itcount(),
            "Expected donor shard to retain an orphaned copy until range deletion runs",
        );

        // Enable per-shard write blocking on shard0 with allowDeletions set to false (i.e., all deletes blocked).
        assert.commandWorked(
            this.shard0PrimaryAdminDB.runCommand({
                blockReplicaSetWrites: 1,
                enabled: true,
                allowDeletions: false,
                reason: "InsufficientDiskSpace",
            }),
        );

        // Let the range deleter run while deletions are blocked and check that the range deleter
        // fails to establish a cursor to delete the range with ReplicaSetWritesBlocked error.
        suspendRangeDel.off();
        checkLog.containsJson(this.st.shard0, 6180602, {error: /ReplicaSetWritesBlocked/}, 60 * 1000);

        // The orphan should still be present since range deletion was blocked.
        assert.eq(
            1,
            shard0LocalColl.find({x: 1}).itcount(),
            "Expected orphan to still be present after range deletion was blocked",
        );

        // Disable the write block, then re-enable with allowDeletions set to true (i.e., all deletes allowed).
        disableReplicaSetWriteBlock(this.shard0PrimaryAdminDB);
        assert.commandWorked(
            this.shard0PrimaryAdminDB.runCommand({
                blockReplicaSetWrites: 1,
                enabled: true,
                allowDeletions: true,
                reason: "InsufficientDiskSpace",
            }),
        );

        // Check that range deletion now completes since allowDeletions is true.
        assert.soon(
            () => shard0LocalColl.find({x: 1}).itcount() === 0,
            "Expected donor orphan to be removed by range deleter when allowDeletions is true",
            5 * 60 * 1000,
            200,
        );
    });
});
