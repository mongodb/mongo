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
 *   # TODO(SERVER-120970): Re-enable this test
 *  __TEMPORARILY_DISABLED__,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {checkLog} from "src/mongo/shell/check_log.js";

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
        if (this.shard0PrimaryConfigDB.replica_set_writes_critical_section.findOne() !== null) {
            assert.commandWorked(
                this.shard0PrimaryAdminDB.runCommand({
                    blockReplicaSetWrites: 1,
                    enabled: false,
                    allowDeletions: false,
                    reason: "InsufficientDiskSpace",
                }),
            );
        }

        if (this.shard0PrimaryConfigDB.user_writes_critical_sections.findOne() !== null) {
            assert.commandWorked(
                this.st.s.adminCommand({
                    setUserWriteBlockMode: 1,
                    global: false,
                    reason: "DiskUseThresholdExceeded",
                }),
            );
        }

        assert.commandWorked(this.testDB.dropDatabase());
    });

    after(function () {
        this.st.stop();
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

        // Disable global user write blocking and check that insert still fails with UserWritesBlocked error.
        assert.commandWorked(
            this.st.s.adminCommand({
                setUserWriteBlockMode: 1,
                global: false,
                reason: "DiskUseThresholdExceeded",
            }),
        );
        assert.commandFailedWithCode(
            this.testShardedColl.insert({x: 4}),
            ErrorCodes.UserWritesBlocked,
            "Expected UserWritesBlocked error after cluster user write block is cleared",
        );

        // Disable per-shard write blocking on shard0 and check that insert succeeds.
        assert.commandWorked(
            this.shard0PrimaryAdminDB.runCommand({
                blockReplicaSetWrites: 1,
                enabled: false,
                allowDeletions: false,
                reason: "InsufficientDiskSpace",
            }),
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

        // Check that insert, update, and delete operations routed to shard0 fail with UserWritesBlocked error.
        assert.commandFailedWithCode(this.testShardedColl.insert({x: -2, y: "c"}), ErrorCodes.UserWritesBlocked);
        assert.commandFailedWithCode(
            this.testShardedColl.update({x: -1}, {$set: {y: "d"}}),
            ErrorCodes.UserWritesBlocked,
        );
        assert.commandFailedWithCode(this.testShardedColl.remove({x: -1}), ErrorCodes.UserWritesBlocked);
        assert.eq(1, this.testShardedColl.find({x: -1, y: "a"}).itcount());

        // Check that insert, update, and delete operations routed to shard1 succeed.
        assert.commandWorked(this.testShardedColl.insert({x: 2, y: "c"}));
        assert.commandWorked(this.testShardedColl.update({x: 1}, {$set: {y: "d"}}));
        assert.commandWorked(this.testShardedColl.remove({x: 2}));
        assert.eq(1, this.testShardedColl.find({x: 1, y: "d"}).itcount());
        assert.eq(0, this.testShardedColl.find({x: 2}).itcount());
    });

    it("Test range deletion on donor cannot complete while blockReplicaSetWrites is enabled", function () {
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

        // Enable per-shard write blocking on shard0.
        assert.commandWorked(
            this.shard0PrimaryAdminDB.runCommand({
                blockReplicaSetWrites: 1,
                enabled: true,
                allowDeletions: false,
                reason: "InsufficientDiskSpace",
            }),
        );

        // Let the range deleter run while deletions are blocked and check that the range deleter
        // fails to establish a cursor to delete the range with UserWritesBlocked error.
        suspendRangeDel.off();
        checkLog.containsJson(this.st.shard0, 6180602, {error: /UserWritesBlocked/}, 60 * 1000);

        // Disable per-shard write blocking on shard0.
        assert.commandWorked(
            this.shard0PrimaryAdminDB.runCommand({
                blockReplicaSetWrites: 1,
                enabled: false,
                allowDeletions: false,
                reason: "InsufficientDiskSpace",
            }),
        );

        // Check that the orphaned document is removed from shard0.
        assert.soon(
            () => shard0LocalColl.find({x: 1}).itcount() === 0,
            "Expected donor orphan to be removed by range deleter after write block is disabled",
            5 * 60 * 1000,
            200,
        );
    });
});
