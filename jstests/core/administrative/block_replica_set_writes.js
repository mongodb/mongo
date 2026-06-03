// Test blockReplicaSetWrites command.
//
// @tags: [
//   requires_persistence,
//   requires_replication,
//   uses_parallel_shell,
//   does_not_support_stepdowns,
//   assumes_against_mongod_not_mongos,
//   assumes_read_concern_unchanged,
//   assumes_read_preference_unchanged,
//   featureFlagBlockReplicaSetWrites
// ]

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {afterEach, before, describe, it} from "jstests/libs/mochalite.js";

function disableReplicaSetWriteBlock(adminDB) {
    assert.soon(() => {
        const res = adminDB.runCommand({
            blockReplicaSetWrites: 1,
            enabled: false,
            reason: "InsufficientDiskSpace",
        });
        return res.ok;
    }, "Failed to disable replica set write block");
}

describe("Test blockReplicaSetWrites command on replica set level", function () {
    before(function () {
        this.replicaSetPrimary = db.getMongo();
        // Extract numeric port for startParallelShell to connect directly to the replica set primary. When
        // using plain connections such as db.getMongo(), there is no metadata layer setting the port, so we
        // need to extract it manually.
        this.replicaSetPrimaryPort = parseInt(this.replicaSetPrimary.host.split(":")[1]);
        this.replicaSetPrimaryAdminDB = this.replicaSetPrimary.getDB("admin");
        this.replicaSetPrimaryConfigDB = this.replicaSetPrimary.getDB("config");
        this.testDbName = "testDB";
        this.adminTestCollName = this.testDbName + "_adminTestColl";
        this.configTestCollName = this.testDbName + "_configTestColl";
    });

    afterEach(function () {
        // Disable replica set and global user write block (if not previously enabled the operation is a no-op).
        disableReplicaSetWriteBlock(this.replicaSetPrimaryAdminDB);
        assert.commandWorked(
            this.replicaSetPrimaryAdminDB.runCommand({
                setUserWriteBlockMode: 1,
                global: false,
                reason: "DiskUseThresholdExceeded",
            }),
        );

        // Remove collections and databases.
        this.replicaSetPrimaryAdminDB.getCollection(this.adminTestCollName).drop();
        this.replicaSetPrimaryConfigDB.getCollection(this.configTestCollName).drop();
        assert.commandWorked(this.replicaSetPrimary.getDB(this.testDbName).dropDatabase());
    });

    it("Test that concurrent blockReplicaSetWrites commands serialize", function () {
        const hangFailPoint = configureFailPoint(this.replicaSetPrimary, "hangInBlockReplicaSetWritesCommand");

        jsTest.log.info("Starting parallel shell to run parallel blockReplicaSetWrites command");
        const awaitShell = startParallelShell(() => {
            assert.commandWorked(
                db.getSiblingDB("admin").runCommand({
                    blockReplicaSetWrites: 1,
                    enabled: true,
                    allowDeletions: false,
                    reason: "InsufficientDiskSpace",
                }),
            );
        }, this.replicaSetPrimaryPort);

        jsTest.log.info("Wait for fail point to be hit");
        hangFailPoint.wait();

        jsTest.log.info(
            "Try to run a second blockReplicaSetWrites command, which should timeout since the first command is still running",
        );
        assert.commandFailedWithCode(
            this.replicaSetPrimaryAdminDB.runCommand({
                blockReplicaSetWrites: 1,
                enabled: true,
                allowDeletions: false,
                reason: "InsufficientDiskSpace",
                maxTimeMS: 5000,
            }),
            ErrorCodes.MaxTimeMSExpired,
        );

        jsTest.log.info("Turn off fail point to allow first command to complete");
        hangFailPoint.off();
        awaitShell();
    });

    it("Test that allowDeletions is required when enabling the block", function () {
        assert.commandFailedWithCode(
            this.replicaSetPrimaryAdminDB.runCommand({
                blockReplicaSetWrites: 1,
                enabled: true,
                reason: "InsufficientDiskSpace",
            }),
            ErrorCodes.InvalidOptions,
            "Expected InvalidOptions when enabling blockReplicaSetWrites without allowDeletions",
        );
    });

    it("Test that the block replica set writes critical section document is persisted correctly", function () {
        assert.commandWorked(
            this.replicaSetPrimaryAdminDB.runCommand({
                blockReplicaSetWrites: 1,
                enabled: true,
                allowDeletions: false,
                reason: "InsufficientDiskSpace",
            }),
        );

        const doc = this.replicaSetPrimaryConfigDB.replica_set_writes_critical_section.findOne();
        assert.eq(true, doc.enabled, "Expected enabled to be true");
        assert.eq(false, doc.allowDeletions, "Expected allowDeletions to be false");
        assert.eq(
            "InsufficientDiskSpace",
            doc.replicaSetWritesBlockReason,
            "Expected replicaSetWritesBlockReason to be InsufficientDiskSpace",
        );

        disableReplicaSetWriteBlock(this.replicaSetPrimaryAdminDB);

        const docsAfterRelease = this.replicaSetPrimaryConfigDB.replica_set_writes_critical_section.find().toArray();
        assert.eq(
            0,
            docsAfterRelease.length,
            "Expected no block replica set writes critical section documents after releasing critical section",
        );
    });

    it("Test CUD operations are blocked/allowed when replica set write block is enabled/disabled", function () {
        const testColl = this.replicaSetPrimary.getDB(this.testDbName).getCollection("testColl");

        assert.commandWorked(testColl.insert({_id: 1, x: 1}));

        // Test CUD operations are blocked after enabling replica set write block
        assert.commandWorked(
            this.replicaSetPrimaryAdminDB.runCommand({
                blockReplicaSetWrites: 1,
                enabled: true,
                allowDeletions: false,
                reason: "InsufficientDiskSpace",
            }),
        );

        assert.commandFailedWithCode(testColl.insert({_id: 2, x: 2}), ErrorCodes.ReplicaSetWritesBlocked);
        assert.commandFailedWithCode(testColl.update({_id: 1}, {$set: {x: 100}}), ErrorCodes.ReplicaSetWritesBlocked);
        assert.commandFailedWithCode(testColl.remove({_id: 1}), ErrorCodes.ReplicaSetWritesBlocked);

        assert.eq(1, testColl.find({_id: 1, x: 1}).itcount());

        // Test CUD operations are allowed after disabling replica set write block
        disableReplicaSetWriteBlock(this.replicaSetPrimaryAdminDB);

        assert.commandWorked(testColl.insert({_id: 2, x: 2}));
        assert.commandWorked(testColl.update({_id: 1}, {$set: {x: 100}}));
        assert.commandWorked(testColl.remove({_id: 2}));

        assert.eq(1, testColl.find({_id: 1, x: 100}).itcount());
    });

    it("Test operations to internal databases are NOT blocked by replica set write block", function () {
        assert.commandWorked(
            this.replicaSetPrimaryAdminDB.runCommand({
                blockReplicaSetWrites: 1,
                enabled: true,
                allowDeletions: false,
                reason: "InsufficientDiskSpace",
            }),
        );
        assert.commandWorked(
            this.replicaSetPrimaryAdminDB.getCollection(this.adminTestCollName).insert({_id: 1, x: 1}),
        );
        assert.commandWorked(
            this.replicaSetPrimaryAdminDB.getCollection(this.adminTestCollName).update({_id: 1}, {$set: {x: 100}}),
        );

        assert.commandWorked(
            this.replicaSetPrimaryConfigDB.getCollection(this.configTestCollName).insert({_id: 1, x: 1}),
        );
        assert.commandWorked(
            this.replicaSetPrimaryConfigDB.getCollection(this.configTestCollName).update({_id: 1}, {$set: {x: 100}}),
        );

        // We do not test operations to local database because the test would fail in
        // retryable_writes_jscore_passthrough suite (retryable writes are not supported on local database).
    });

    it("Test operations to system.profile collections are NOT blocked by replica set write block", function () {
        const testDB = this.replicaSetPrimary.getDB(this.testDbName);
        const testColl = testDB.getCollection("testColl");
        assert.commandWorked(testColl.insert({_id: 1}));

        // Profiling is not supported in some configurations (e.g. Disaggregated Storage), so we skip the test in that case.
        const enableProfileRes = testDB.runCommand({profile: 2});
        if (!enableProfileRes.ok && enableProfileRes.code === ErrorCodes.CommandNotSupported) {
            jsTest.log.info(
                "Skipping system.profile testing since profiling is not supported: " + tojsononeline(enableProfileRes),
            );
            return;
        }

        assert.commandWorked(
            this.replicaSetPrimaryAdminDB.runCommand({
                blockReplicaSetWrites: 1,
                enabled: true,
                allowDeletions: false,
                reason: "InsufficientDiskSpace",
            }),
        );

        // Enable profiling and find the unique comment in system.profile to verify writes are logged.
        const comment = "replicaSetProfileTest";
        testColl.find().comment(comment).itcount();
        assert.soon(
            () => testDB.system.profile.find({"command.comment": comment}).itcount() === 1,
            "Expected one system.profile entry for comment " + comment,
        );

        assert.commandWorked(testDB.runCommand({profile: 0}));
    });

    it("Test user deletions are blocked when allowDeletions is false and allowed when allowDeletions is true", function () {
        const testColl = this.replicaSetPrimary.getDB(this.testDbName).getCollection("testColl");

        assert.commandWorked(testColl.insert({_id: 1}));
        assert.commandWorked(testColl.insert({_id: 2}));

        // Enable per-shard write blocking and check that user deletes are blocked.
        assert.commandWorked(
            this.replicaSetPrimaryAdminDB.runCommand({
                blockReplicaSetWrites: 1,
                enabled: true,
                allowDeletions: false,
                reason: "InsufficientDiskSpace",
            }),
        );
        assert.commandFailedWithCode(testColl.remove({_id: 1}), ErrorCodes.ReplicaSetWritesBlocked);
        assert.eq(2, testColl.count(), "Both documents should remain while deletions are blocked");

        // Disable write block and and re-enable with allowDeletions: true to check that user deletes are allowed.
        disableReplicaSetWriteBlock(this.replicaSetPrimaryAdminDB);
        assert.commandWorked(
            this.replicaSetPrimaryAdminDB.runCommand({
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

    it("Test TTL deletions are blocked when allowDeletions is false and allowed when allowDeletions is true", function () {
        const replicaSetPrimaryAdminDB = this.replicaSetPrimaryAdminDB;
        function runTTLMonitor() {
            const ttlPass = replicaSetPrimaryAdminDB.serverStatus().metrics.ttl.passes;
            assert.commandWorked(
                replicaSetPrimaryAdminDB.runCommand({
                    configureFailPoint: "hangTTLMonitorBetweenPasses",
                    mode: {skip: 1},
                }),
            );
            assert.soon(
                () => replicaSetPrimaryAdminDB.serverStatus().metrics.ttl.passes >= ttlPass + 1,
                "TTL monitor didn't run before timing out.",
            );
        }

        const testColl = this.replicaSetPrimary.getDB(this.testDbName).getCollection("testColl");

        // Pause the TTL monitor so it doesn't run during setup.
        const pauseTtl = configureFailPoint(this.replicaSetPrimary, "hangTTLMonitorBetweenPasses");
        pauseTtl.wait();

        // Insert an already-expired document and create a TTL index with expireAfterSeconds: 0.
        assert.commandWorked(testColl.insert({_id: 1, createdAt: new Date(0)}));
        assert.commandWorked(testColl.createIndex({createdAt: 1}, {expireAfterSeconds: 0}));

        // Enable write block with allowDeletions: false and let TTL run — document should not be reaped.
        assert.commandWorked(
            this.replicaSetPrimaryAdminDB.runCommand({
                blockReplicaSetWrites: 1,
                enabled: true,
                allowDeletions: false,
                reason: "InsufficientDiskSpace",
            }),
        );
        runTTLMonitor();
        assert.eq(1, testColl.count(), "TTL should not have reaped the document while deletions are blocked");

        // Disable write block and re-enable with allowDeletions: true — TTL should now reap the document.
        disableReplicaSetWriteBlock(this.replicaSetPrimaryAdminDB);
        assert.commandWorked(
            this.replicaSetPrimaryAdminDB.runCommand({
                blockReplicaSetWrites: 1,
                enabled: true,
                allowDeletions: true,
                reason: "InsufficientDiskSpace",
            }),
        );
        runTTLMonitor();
        assert.eq(0, testColl.count(), "TTL should have reaped the document when allowDeletions is true");

        pauseTtl.off();
    });

    it("Test that DDLs are not blocked by replica set write block", function () {
        const testDB = this.replicaSetPrimary.getDB(this.testDbName);
        const testColl0 = testDB.getCollection("testColl0");
        assert.commandWorked(testColl0.insert({_id: 1}));

        assert.commandWorked(
            this.replicaSetPrimaryAdminDB.runCommand({
                blockReplicaSetWrites: 1,
                enabled: true,
                allowDeletions: false,
                reason: "InsufficientDiskSpace",
            }),
        );

        assert.commandWorked(testColl0.renameCollection("renamedTestColl0"));
        assert.commandWorked(testDB.createCollection("testColl1"));
        assert.commandWorked(testDB.testColl1.createIndex({a: 1}, {name: "idx"}));
        assert.commandWorked(testDB.testColl1.dropIndex("idx"));
    });

    it("Test that when global and replica set write blocks are enabled concurrently, writes return the global error code", function () {
        const testDB = this.replicaSetPrimary.getDB(this.testDbName);
        const testColl = testDB.getCollection("testColl");
        assert.commandWorked(testColl.insert({_id: 1, x: 0}));

        assert.commandWorked(
            this.replicaSetPrimaryAdminDB.runCommand({
                blockReplicaSetWrites: 1,
                enabled: true,
                allowDeletions: false,
                reason: "InsufficientDiskSpace",
            }),
        );
        assert.commandWorked(
            this.replicaSetPrimaryAdminDB.runCommand({
                setUserWriteBlockMode: 1,
                global: true,
                reason: "DiskUseThresholdExceeded",
            }),
        );

        // With both blocks active, writes must fail with the global error code (UserWritesBlocked),
        // not ReplicaSetWritesBlocked.
        const insertRes = assert.commandFailedWithCode(testColl.insert({_id: 2}), ErrorCodes.UserWritesBlocked);
        assert(
            insertRes.getWriteError().errmsg.includes("DiskUseThresholdExceeded"),
            "Expected reason DiskUseThresholdExceeded in error message",
            {insertRes},
        );
        const updateRes = assert.commandFailedWithCode(
            testColl.update({_id: 1}, {$set: {x: 1}}),
            ErrorCodes.UserWritesBlocked,
        );
        assert(
            updateRes.getWriteError().errmsg.includes("DiskUseThresholdExceeded"),
            "Expected reason DiskUseThresholdExceeded in error message",
            {updateRes},
        );
        const deleteRes = assert.commandFailedWithCode(testColl.remove({_id: 1}), ErrorCodes.UserWritesBlocked);
        assert(
            deleteRes.getWriteError().errmsg.includes("DiskUseThresholdExceeded"),
            "Expected reason DiskUseThresholdExceeded in error message",
            {deleteRes},
        );
    });

    it("Test interaction between global write block and replica set write block (disable global write block first)", function () {
        const testDB = this.replicaSetPrimary.getDB(this.testDbName);
        const testColl0 = testDB.getCollection("testColl0");

        // Enable global and replica set write block.
        assert.commandWorked(
            this.replicaSetPrimaryAdminDB.runCommand({
                blockReplicaSetWrites: 1,
                enabled: true,
                allowDeletions: false,
                reason: "InsufficientDiskSpace",
            }),
        );
        assert.commandWorked(
            this.replicaSetPrimaryAdminDB.runCommand({
                setUserWriteBlockMode: 1,
                global: true,
                reason: "DiskUseThresholdExceeded",
            }),
        );

        // CUD should be blocked and return DiskUseThresholdExceeded reason.
        const res = assert.commandFailedWithCode(testColl0.insert({_id: 1}), ErrorCodes.UserWritesBlocked);
        assert(
            res.getWriteError().errmsg.includes("DiskUseThresholdExceeded"),
            "Expected reason DiskUseThresholdExceeded in error message",
        );

        // DDL should now also be blocked (global blocks DDL).
        assert.commandFailedWithCode(testDB.createCollection("testColl1"), ErrorCodes.UserWritesBlocked);

        // Disable global write blocking and check that DDLs are allowed again. After the previous blocked
        // createCollection operation, setUserWriteBlockMode command can throw WriteConflict, so we need to
        // retry until it succeeds.
        assert.soon(() => {
            const res = this.replicaSetPrimaryAdminDB.runCommand({
                setUserWriteBlockMode: 1,
                global: false,
                reason: "DiskUseThresholdExceeded",
            });
            if (!res.ok) {
                return false;
            }
            return true;
        }, "Failed to disable global write blocking");
        assert.commandWorked(testDB.createCollection("testColl1"));
        assert(testDB.testColl1.drop());

        // CUD still blocked by replica set write block.
        assert.commandFailedWithCode(
            testDB.getCollection("testColl0").insert({_id: 2}),
            ErrorCodes.ReplicaSetWritesBlocked,
        );
    });

    it("Test interaction between global write block and replica set write block (disable replica set write block first)", function () {
        const testDB = this.replicaSetPrimary.getDB(this.testDbName);
        const testColl0 = testDB.getCollection("testColl0");

        // Enable global write block first, then replica set write block.
        assert.commandWorked(
            this.replicaSetPrimaryAdminDB.runCommand({
                setUserWriteBlockMode: 1,
                global: true,
                reason: "DiskUseThresholdExceeded",
            }),
        );
        assert.commandWorked(
            this.replicaSetPrimaryAdminDB.runCommand({
                blockReplicaSetWrites: 1,
                enabled: true,
                allowDeletions: false,
                reason: "InsufficientDiskSpace",
            }),
        );

        // CUD should be blocked. UserWriteBlockModeOpObserver fires before
        // ReplicaSetWriteBlockOpObserver, so the global reason surfaces regardless of enable order.
        const res = assert.commandFailedWithCode(testColl0.insert({_id: 1}), ErrorCodes.UserWritesBlocked);
        assert(
            res.getWriteError().errmsg.includes("DiskUseThresholdExceeded"),
            "Expected reason DiskUseThresholdExceeded in error message",
            {res},
        );

        // DDL should also be blocked (global blocks DDL).
        assert.commandFailedWithCode(testDB.createCollection("testColl1"), ErrorCodes.UserWritesBlocked);

        // Disable replica set write block. Global block remains, so CUD and DDL stay blocked.
        assert.commandWorked(
            this.replicaSetPrimaryAdminDB.runCommand({
                blockReplicaSetWrites: 1,
                enabled: false,
                reason: "InsufficientDiskSpace",
            }),
        );
        assert.commandFailedWithCode(testColl0.insert({_id: 2}), ErrorCodes.UserWritesBlocked);
        assert.commandFailedWithCode(testDB.createCollection("testColl1"), ErrorCodes.UserWritesBlocked);

        // Disable global write block. Both CUD and DDL should now be allowed.
        assert.soon(() => {
            const res = this.replicaSetPrimaryAdminDB.runCommand({
                setUserWriteBlockMode: 1,
                global: false,
                reason: "DiskUseThresholdExceeded",
            });
            return res.ok;
        }, "Failed to disable global write blocking");
        assert.commandWorked(testColl0.insert({_id: 3}));
        assert.commandWorked(testDB.createCollection("testColl1"));
        assert(testDB.testColl1.drop());
    });
});
