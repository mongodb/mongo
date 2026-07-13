// Test blockReplicaSetWrites command.
//
// @tags: [
//   requires_persistence,
//   requires_replication,
//   uses_parallel_shell,
//   does_not_support_stepdowns,
//   assumes_read_preference_unchanged,
//   assumes_against_mongod_not_mongos,
//   featureFlagBlockReplicaSetWrites,
//   # TODO(SERVER-123007): once 9.0 is lastLTS, remove this tag.
//   multiversion_incompatible,
//   does_not_support_config_fuzzer,
//   cannot_run_during_upgrade_downgrade,
//   wildcard_indexes_incompatible,
// ]

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {
    disableReplicaSetWriteBlock,
    enableReplicaSetWriteBlock,
} from "jstests/libs/block_replica_set_writes_utils.js";
import {afterEach, before, describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {PersistenceProviderUtil} from "jstests/libs/server-rss/persistence_provider_util.js";
import {isEnterpriseShell, runEncryptedTest} from "jstests/fle2/libs/encrypted_client_util.js";

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
        disableReplicaSetWriteBlock(
            this.replicaSetPrimaryAdminDB,
            "InsufficientDiskSpace" /* reason */,
        );
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

    it("Test that concurrent blockReplicaSetWrites invocations are serialized", function () {
        const primary = this.replicaSetPrimary;
        const failPointName = "hangInBlockReplicaSetWritesCommand";

        // Pause the first invocation inside the critical section, while it holds the global
        // serialization mutex.
        const fp = configureFailPoint(primary, failPointName);

        let enableShell;
        let disableShell;
        try {
            // First invocation: enable the block. It acquires the global mutex and hangs at the
            // failpoint.
            enableShell = startParallelShell(
                funWithArgs(function (reason) {
                    assert.commandWorked(
                        db.getSiblingDB("admin").runCommand({
                            blockReplicaSetWrites: 1,
                            enabled: true,
                            allowDeletions: false,
                            reason: reason,
                        }),
                    );
                }, "InsufficientDiskSpace"),
                this.replicaSetPrimaryPort,
            );

            // Wait until the first invocation is parked at the failpoint. The command evaluates
            // the failpoint via shouldFail() and then parks in pauseWhileSet(), which enters it a
            // second time, so the parked invocation accounts for exactly two hits.
            const hitsPerInvocation = 2;
            const parkedHits = fp.timesEntered + hitsPerInvocation;
            fp.wait({timesEntered: hitsPerInvocation});

            // Second invocation: disable the block. Because the mutex is process-wide, this
            // invocation must block on mutex acquisition and therefore cannot reach the
            // failpoint-guarded critical section while the first invocation holds the lock.
            disableShell = startParallelShell(
                funWithArgs(function (reason) {
                    assert.commandWorked(
                        db.getSiblingDB("admin").runCommand({
                            blockReplicaSetWrites: 1,
                            enabled: false,
                            reason: reason,
                        }),
                    );
                }, "InsufficientDiskSpace"),
                this.replicaSetPrimaryPort,
            );

            // The second invocation must not enter the failpoint while the first holds the mutex,
            // so the hit count must not advance past the parked snapshot.
            assert.commandFailedWithCode(
                primary.adminCommand({
                    waitForFailPoint: failPointName,
                    timesEntered: parkedHits + 1,
                    maxTimeMS: 5 * 1000,
                }),
                ErrorCodes.MaxTimeMSExpired,
                "Second blockReplicaSetWrites invocation should be serialized behind the first and not enter the critical section",
            );
        } finally {
            // Always release the first invocation so it drops the mutex, even if an assertion above
            // threw. Otherwise the parked invocation would hold the mutex and deadlock cleanup.
            fp.off();
        }

        // Release the parallel shells. The first finishes enabling the block and drops the mutex,
        // after which the second acquires it and disables the block.
        enableShell();
        disableShell();

        // After both invocations complete, the block should be disabled (the second invocation ran last).
        const replStatus = assert.commandWorked(this.replicaSetPrimaryAdminDB.serverStatus()).repl;
        assert.eq(
            replStatus.replicaSetWriteBlock,
            1,
            "replicaSetWriteBlock metric should be 1 (Disabled) after the serialized disable completes",
        );
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
        enableReplicaSetWriteBlock(
            this.replicaSetPrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );

        const doc = this.replicaSetPrimaryConfigDB.replica_set_writes_critical_section.findOne();
        assert.eq(true, doc.enabled, "Expected enabled to be true");
        assert.eq(false, doc.allowDeletions, "Expected allowDeletions to be false");
        assert.eq(
            "InsufficientDiskSpace",
            doc.replicaSetWritesBlockReason,
            "Expected replicaSetWritesBlockReason to be InsufficientDiskSpace",
        );

        disableReplicaSetWriteBlock(
            this.replicaSetPrimaryAdminDB,
            "InsufficientDiskSpace" /* reason */,
        );

        const docsAfterRelease = this.replicaSetPrimaryConfigDB.replica_set_writes_critical_section
            .find()
            .toArray();
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
        enableReplicaSetWriteBlock(
            this.replicaSetPrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );

        assert.commandFailedWithCode(
            testColl.insert({_id: 2, x: 2}),
            ErrorCodes.ReplicaSetWritesBlocked,
        );
        assert.commandFailedWithCode(
            testColl.update({_id: 1}, {$set: {x: 100}}),
            ErrorCodes.ReplicaSetWritesBlocked,
        );
        assert.commandFailedWithCode(testColl.remove({_id: 1}), ErrorCodes.ReplicaSetWritesBlocked);

        assert.eq(1, testColl.find({_id: 1, x: 1}).itcount());

        // Test CUD operations are allowed after disabling replica set write block
        disableReplicaSetWriteBlock(
            this.replicaSetPrimaryAdminDB,
            "InsufficientDiskSpace" /* reason */,
        );

        assert.commandWorked(testColl.insert({_id: 2, x: 2}));
        assert.commandWorked(testColl.update({_id: 1}, {$set: {x: 100}}));
        assert.commandWorked(testColl.remove({_id: 2}));

        assert.eq(1, testColl.find({_id: 1, x: 100}).itcount());
    });

    it("Test operations to internal databases are NOT blocked by replica set write block", function () {
        enableReplicaSetWriteBlock(
            this.replicaSetPrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );
        assert.commandWorked(
            this.replicaSetPrimaryAdminDB
                .getCollection(this.adminTestCollName)
                .insert({_id: 1, x: 1}),
        );
        assert.commandWorked(
            this.replicaSetPrimaryAdminDB
                .getCollection(this.adminTestCollName)
                .update({_id: 1}, {$set: {x: 100}}),
        );

        assert.commandWorked(
            this.replicaSetPrimaryConfigDB
                .getCollection(this.configTestCollName)
                .insert({_id: 1, x: 1}),
        );
        assert.commandWorked(
            this.replicaSetPrimaryConfigDB
                .getCollection(this.configTestCollName)
                .update({_id: 1}, {$set: {x: 100}}),
        );

        // We do not test operations to local database because the test would fail in
        // retryable_writes_jscore_passthrough suite (retryable writes are not supported on local database).
    });

    it("Test operations to system.profile collections are NOT blocked by replica set write block", function () {
        const testDB = this.replicaSetPrimary.getDB(this.testDbName);
        const testColl = testDB.getCollection("testColl");
        assert.commandWorked(testColl.insert({_id: 1}));

        // Profiling is not supported in some configurations (e.g. Disaggregated Storage) or in
        // suites that apply the secondary read-preference override (which rejects profile commands
        // because system.profile is not replicated).
        let enableProfileRes;
        try {
            enableProfileRes = testDB.runCommand({profile: 2});
        } catch (e) {
            jsTest.log.info("Skipping system.profile testing since profiling is not supported", {
                e,
            });
            return;
        }
        if (!enableProfileRes.ok) {
            const knownUnsupportedCodes = [
                ErrorCodes.CommandNotSupported,
                ErrorCodes.InvalidOptions,
            ];
            assert(
                knownUnsupportedCodes.includes(enableProfileRes.code),
                "Unexpected error enabling profiling",
                {
                    enableProfileRes,
                },
            );
            jsTest.log.info("Skipping system.profile testing since profiling is not supported", {
                enableProfileRes,
            });
            return;
        }

        enableReplicaSetWriteBlock(
            this.replicaSetPrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
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
        enableReplicaSetWriteBlock(
            this.replicaSetPrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );
        assert.commandFailedWithCode(testColl.remove({_id: 1}), ErrorCodes.ReplicaSetWritesBlocked);
        assert.eq(2, testColl.count(), "Both documents should remain while deletions are blocked");

        // Disable write block and and re-enable with allowDeletions: true to check that user deletes are allowed.
        disableReplicaSetWriteBlock(
            this.replicaSetPrimaryAdminDB,
            "InsufficientDiskSpace" /* reason */,
        );
        enableReplicaSetWriteBlock(
            this.replicaSetPrimaryAdminDB,
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
        enableReplicaSetWriteBlock(
            this.replicaSetPrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );
        runTTLMonitor();
        assert.eq(
            1,
            testColl.count(),
            "TTL should not have reaped the document while deletions are blocked",
        );

        // Disable write block and re-enable with allowDeletions: true — TTL should now reap the document.
        disableReplicaSetWriteBlock(
            this.replicaSetPrimaryAdminDB,
            "InsufficientDiskSpace" /* reason */,
        );
        enableReplicaSetWriteBlock(
            this.replicaSetPrimaryAdminDB,
            true /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );
        runTTLMonitor();
        assert.eq(
            0,
            testColl.count(),
            "TTL should have reaped the document when allowDeletions is true",
        );

        pauseTtl.off();
    });

    it("Test that DDLs are not blocked by replica set write block", function () {
        const testDB = this.replicaSetPrimary.getDB(this.testDbName);
        const testColl0 = testDB.getCollection("testColl0");
        assert.commandWorked(testColl0.insert({_id: 1}));

        enableReplicaSetWriteBlock(
            this.replicaSetPrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
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

        enableReplicaSetWriteBlock(
            this.replicaSetPrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
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
        const insertRes = assert.commandFailedWithCode(
            testColl.insert({_id: 2}),
            ErrorCodes.UserWritesBlocked,
        );
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
        const deleteRes = assert.commandFailedWithCode(
            testColl.remove({_id: 1}),
            ErrorCodes.UserWritesBlocked,
        );
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
        enableReplicaSetWriteBlock(
            this.replicaSetPrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );
        assert.commandWorked(
            this.replicaSetPrimaryAdminDB.runCommand({
                setUserWriteBlockMode: 1,
                global: true,
                reason: "DiskUseThresholdExceeded",
            }),
        );

        // CUD should be blocked and return DiskUseThresholdExceeded reason.
        const res = assert.commandFailedWithCode(
            testColl0.insert({_id: 1}),
            ErrorCodes.UserWritesBlocked,
        );
        assert(
            res.getWriteError().errmsg.includes("DiskUseThresholdExceeded"),
            "Expected reason DiskUseThresholdExceeded in error message",
        );

        // DDL should now also be blocked (global blocks DDL).
        assert.commandFailedWithCode(
            testDB.createCollection("testColl1"),
            ErrorCodes.UserWritesBlocked,
        );

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

    it("Test that compact is guarded by the allowDeletions flag of blockReplicaSetWrites", function () {
        // Skip test function if the architecture does not support auto-compact operations.
        if (
            PersistenceProviderUtil.allNodesHavePropertyWithValue(
                this.replicaSetPrimary,
                "supportsLocalCollections",
                false,
            )
        ) {
            jsTest.log.info(
                "Skipping 'Test that compact on a shard is guarded by the allowDeletions flag of blockReplicaSetWrites' test function because local collections are not supported",
            );
            return;
        }

        const testDB = this.replicaSetPrimary.getDB(this.testDbName);
        const testColl = testDB.getCollection("testColl");
        assert.commandWorked(testColl.insert({_id: 1}));

        // Check that with allowDeletions:false, compact is rejected.
        enableReplicaSetWriteBlock(
            this.replicaSetPrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );
        assert.commandFailedWithCode(
            testDB.runCommand({compact: "testColl", force: true}),
            ErrorCodes.ReplicaSetWritesBlocked,
        );

        // Disable write block.
        disableReplicaSetWriteBlock(
            this.replicaSetPrimaryAdminDB,
            "InsufficientDiskSpace" /* reason */,
        );

        // Check that with allowDeletions:true, compact is permitted.
        enableReplicaSetWriteBlock(
            this.replicaSetPrimaryAdminDB,
            true /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );
        assert.commandWorked(testDB.runCommand({compact: "testColl", force: true}));

        // Disable write block entirely and verify compact still succeeds.
        disableReplicaSetWriteBlock(
            this.replicaSetPrimaryAdminDB,
            "InsufficientDiskSpace" /* reason */,
        );
        assert.commandWorked(testDB.runCommand({compact: "testColl", force: true}));
    });

    it("Test that autoCompact is guarded by the allowDeletions flag while disabling is always allowed", function () {
        // Skip test function if the architecture does not support auto-compact operations.
        if (
            PersistenceProviderUtil.allNodesHavePropertyWithValue(
                this.replicaSetPrimary,
                "supportsLocalCollections",
                false,
            )
        ) {
            jsTest.log.info(
                "Skipping 'Test that autoCompact is guarded by the allowDeletions flag of blockReplicaSetWrites' test function because local collections are not supported",
            );
            return;
        }

        // Check that with allowDeletions:false, auto-compact is rejected.
        enableReplicaSetWriteBlock(
            this.replicaSetPrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );
        assert.commandFailedWithCode(
            this.replicaSetPrimaryAdminDB.runCommand({autoCompact: true}),
            ErrorCodes.ReplicaSetWritesBlocked,
        );

        // Leave background compaction off.
        assert.commandWorked(this.replicaSetPrimaryAdminDB.runCommand({autoCompact: false}));

        // Disable write block.
        disableReplicaSetWriteBlock(
            this.replicaSetPrimaryAdminDB,
            "InsufficientDiskSpace" /* reason */,
        );

        // Check that with allowDeletions:true, auto-compact is permitted.
        enableReplicaSetWriteBlock(
            this.replicaSetPrimaryAdminDB,
            true /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );
        assert.soon(() => {
            const res = this.replicaSetPrimaryAdminDB.runCommand({autoCompact: true});
            if (res.code === ErrorCodes.ObjectIsBusy) return false;
            assert.commandWorked(res);
            return true;
        }, "Timed out waiting to enable autoCompact");

        // Leave background compaction off.
        assert.soon(() => {
            const res = this.replicaSetPrimaryAdminDB.runCommand({autoCompact: false});
            if (res.code === ErrorCodes.ObjectIsBusy) return false;
            assert.commandWorked(res);
            return true;
        }, "Timed out waiting to disable autoCompact");
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
        enableReplicaSetWriteBlock(
            this.replicaSetPrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );

        // CUD should be blocked. UserWriteBlockModeOpObserver fires before
        // ReplicaSetWriteBlockOpObserver, so the global reason surfaces regardless of enable order.
        const res = assert.commandFailedWithCode(
            testColl0.insert({_id: 1}),
            ErrorCodes.UserWritesBlocked,
        );
        assert(
            res.getWriteError().errmsg.includes("DiskUseThresholdExceeded"),
            "Expected reason DiskUseThresholdExceeded in error message",
            {res},
        );

        // DDL should also be blocked (global blocks DDL).
        assert.commandFailedWithCode(
            testDB.createCollection("testColl1"),
            ErrorCodes.UserWritesBlocked,
        );

        // Disable replica set write block. Global block remains, so CUD and DDL stay blocked.
        disableReplicaSetWriteBlock(
            this.replicaSetPrimaryAdminDB,
            "InsufficientDiskSpace" /* reason */,
        );
        assert.commandFailedWithCode(testColl0.insert({_id: 2}), ErrorCodes.UserWritesBlocked);
        assert.commandFailedWithCode(
            testDB.createCollection("testColl1"),
            ErrorCodes.UserWritesBlocked,
        );

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

    it("Test that index builds on empty collections bypass the replica set write block", function () {
        const testDB = this.replicaSetPrimary.getDB(this.testDbName);
        const emptyColl = testDB.getCollection("emptyColl");
        const nonEmptyColl = testDB.getCollection("nonEmptyColl");
        assert.commandWorked(nonEmptyColl.insert({_id: 1, a: 1}));

        enableReplicaSetWriteBlock(this.replicaSetPrimaryAdminDB, false, "InsufficientDiskSpace");

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

    it("Test that new index builds on user collections are blocked when blockReplicaSetWrites is enabled", function () {
        const testDB = this.replicaSetPrimary.getDB(this.testDbName);
        const testColl = testDB.getCollection("testColl");
        assert.commandWorked(testColl.insert({_id: 1, a: 1}));

        enableReplicaSetWriteBlock(this.replicaSetPrimaryAdminDB, false, "InsufficientDiskSpace");

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
        disableReplicaSetWriteBlock(this.replicaSetPrimaryAdminDB, "InsufficientDiskSpace");
        assert.commandWorked(testColl.createIndex({a: 1}));
        assert.neq(
            null,
            testColl.getIndexes().find((idx) => idx.name === "a_1"),
            "Expected index to exist after write block is disabled",
        );
    });

    it("Test that in-progress index builds before the commit phase are aborted when blockReplicaSetWrites is enabled", function () {
        const testDB = this.replicaSetPrimary.getDB(this.testDbName);
        const testColl = testDB.getCollection("inProgressIndexBuildColl");
        assert.commandWorked(testColl.insert({_id: 1, a: 1}));

        // Verify that index builds succeed normally before write blocking is enabled.
        assert.commandWorked(testColl.createIndex({a: 1}));
        assert.neq(
            null,
            testColl.getIndexes().find((idx) => idx.name === "a_1"),
            "Expected index to exist before write blocking is enabled",
        );
        assert.commandWorked(testColl.dropIndex("a_1"));

        // Pause the index build after initialization so we can enable the write block while it is in progress.
        const hangFp = configureFailPoint(
            this.replicaSetPrimary,
            "hangAfterInitializingIndexBuild",
        );

        // Start the index build in a parallel shell.
        const awaitIndexBuild = startParallelShell(() => {
            const res = assert.commandFailedWithCode(
                db
                    .getSiblingDB("testDB")
                    .getCollection("inProgressIndexBuildColl")
                    .createIndex({a: 1}),
                [ErrorCodes.IndexBuildAborted, ErrorCodes.ReplicaSetWritesBlocked],
            );
            // The build must fail because of write blocking. This surfaces in one of two ways: the
            // in-progress build is aborted with a "Write blocking" reason (IndexBuildAborted), or the
            // build's setup write is rejected outright with "Replica set write blocked"
            // (ReplicaSetWritesBlocked) when the build restarts after the block is already enabled
            // (e.g. when a concurrent FCV change interrupts the paused build).
            assert(
                res.errmsg.includes("Write blocking") ||
                    res.errmsg.includes("Replica set write blocked"),
                "Unexpected index build failure reason",
                {res},
            );
        }, this.replicaSetPrimaryPort);

        hangFp.wait();

        // Enable replica set write blocking while the index build is paused.
        enableReplicaSetWriteBlock(this.replicaSetPrimaryAdminDB, false, "InsufficientDiskSpace");

        hangFp.off();
        awaitIndexBuild();

        // Verify the index was not created.
        assert.eq(
            null,
            testColl.getIndexes().find((idx) => idx.name === "a_1"),
            "Expected index to not exist after in-progress build was aborted",
        );

        // Disable write blocking and verify that index builds succeed again.
        disableReplicaSetWriteBlock(this.replicaSetPrimaryAdminDB, "InsufficientDiskSpace");
        assert.commandWorked(testColl.createIndex({a: 1}));
        assert.neq(
            null,
            testColl.getIndexes().find((idx) => idx.name === "a_1"),
            "Expected index to exist after write blocking is disabled",
        );
    });

    it("Test that an index build already in the commit phase is not aborted but waited on when blockReplicaSetWrites is enabled", function () {
        const testDB = this.replicaSetPrimary.getDB(this.testDbName);
        const testColl = testDB.getCollection("commitPhaseIndexBuildColl");
        assert.commandWorked(testColl.insert({_id: 1, a: 1}));

        // Pause the index build once it has reached the commit phase. At this point the build is
        // past the point where it can be aborted, so enabling write blocking must wait for it to
        // finish rather than abort it.
        const hangFp = configureFailPoint(this.replicaSetPrimary, "hangIndexBuildBeforeCommit");

        // Start the index build; it should succeed (not be aborted).
        const awaitIndexBuild = startParallelShell(() => {
            assert.commandWorked(
                db
                    .getSiblingDB("testDB")
                    .getCollection("commitPhaseIndexBuildColl")
                    .createIndex({a: 1}),
            );
        }, this.replicaSetPrimaryPort);

        hangFp.wait();

        // Enable replica set write blocking in a parallel shell. Because the index build is already
        // committing and cannot be aborted, the command must wait for the build to finish before
        // it completes.
        const awaitWriteBlock = startParallelShell(() => {
            assert.commandWorked(
                db.getSiblingDB("admin").runCommand({
                    blockReplicaSetWrites: 1,
                    enabled: true,
                    allowDeletions: false,
                    reason: "InsufficientDiskSpace",
                }),
            );
        }, this.replicaSetPrimaryPort);

        // While the index build is still paused in commit, the write-blocking command must not have
        // finished: it is blocked waiting on the build that it could not abort.
        assert.soon(
            () =>
                this.replicaSetPrimaryAdminDB
                    .aggregate([
                        {$currentOp: {}},
                        {$match: {"command.blockReplicaSetWrites": {$exists: true}}},
                    ])
                    .toArray().length > 0,
            "Expected blockReplicaSetWrites to still be waiting on the committing index build",
        );

        // Let the index build commit; both the build and the write-blocking command should finish.
        hangFp.off();
        awaitIndexBuild();
        awaitWriteBlock();

        // Verify the index was created: a commit-phase build must not be aborted.
        assert.neq(
            null,
            testColl.getIndexes().find((idx) => idx.name === "a_1"),
            "Expected index to exist.",
        );
    });

    it("Test that new cloneCollectionAsCapped is blocked by replica set write block", function () {
        const testDB = this.replicaSetPrimary.getDB(this.testDbName);
        const srcColl = testDB.getCollection("srcColl");
        assert.commandWorked(srcColl.insert([{_id: 1}, {_id: 2}]));

        // Enable replica set write block and check that cloneCollectionAsCapped is blocked.
        enableReplicaSetWriteBlock(
            this.replicaSetPrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );
        assert.commandFailedWithCode(
            testDB.runCommand({
                cloneCollectionAsCapped: "srcColl",
                toCollection: "dstColl",
                size: 1024 * 1024,
            }),
            ErrorCodes.ReplicaSetWritesBlocked,
        );

        // Disable write block and check that cloneCollectionAsCapped succeeds.
        disableReplicaSetWriteBlock(
            this.replicaSetPrimaryAdminDB,
            "InsufficientDiskSpace" /* reason */,
        );
        assert.commandWorked(
            testDB.runCommand({
                cloneCollectionAsCapped: "srcColl",
                toCollection: "dstColl",
                size: 1024 * 1024,
            }),
        );
        assert.eq(2, testDB.getCollection("dstColl").countDocuments({}));
    });

    it("Test that new QE compact and cleanup are blocked when allowDeletions: false", function () {
        // blockedColl is a non-existent collection, so when the commands are not blocked we expect
        // them to fail with NamespaceNotFound. The deletions check fires before collection validation, so a
        // non-existent collection is sufficient to verify the rejection.
        const testDB = this.replicaSetPrimary.getDB(this.testDbName);
        const compactCmd = {compactStructuredEncryptionData: "blockedColl", compactionTokens: {}};
        const cleanupCmd = {cleanupStructuredEncryptionData: "blockedColl", cleanupTokens: {}};

        // Enable replica set write block with allowDeletions: false and check that the commands are blocked.
        enableReplicaSetWriteBlock(
            this.replicaSetPrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );
        assert.commandFailedWithCode(
            testDB.runCommand(compactCmd),
            ErrorCodes.ReplicaSetWritesBlocked,
        );
        assert.commandFailedWithCode(
            testDB.runCommand(cleanupCmd),
            ErrorCodes.ReplicaSetWritesBlocked,
        );

        // Disable replica set write block and check that new QE compact and cleanup are not blocked anymore.
        disableReplicaSetWriteBlock(
            this.replicaSetPrimaryAdminDB,
            "InsufficientDiskSpace" /* reason */,
        );
        assert.commandFailedWithCode(testDB.runCommand(compactCmd), ErrorCodes.NamespaceNotFound);
        assert.commandFailedWithCode(testDB.runCommand(cleanupCmd), ErrorCodes.NamespaceNotFound);
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
        runEncryptedTest(db, fleDbName, fleCollName, encryptedFields, (edb, client) => {
            const coll = edb[fleCollName];
            for (let i = 0; i < 5; i++) {
                assert.commandWorked(coll.insert({first: "roger_" + i}));
            }
            try {
                enableReplicaSetWriteBlock(
                    this.replicaSetPrimaryAdminDB,
                    true /* allowDeletions */,
                    "InsufficientDiskSpace",
                );
                // Check that compact and cleanup complete successfully when allowDeletions: true.
                assert.commandWorked(coll.compact());
                assert.commandWorked(coll.cleanup());
            } finally {
                disableReplicaSetWriteBlock(this.replicaSetPrimaryAdminDB, "InsufficientDiskSpace");
            }
        });
    });

    it("Test that new convertToCapped is blocked when replica set write block is enabled", function () {
        const testDB = this.replicaSetPrimary.getDB(this.testDbName);
        const testColl = testDB.getCollection("testColl");
        assert.commandWorked(testColl.insert({_id: 1}));

        // Enable write block with allowDeletions:false and check that convertToCapped is rejected.
        enableReplicaSetWriteBlock(
            this.replicaSetPrimaryAdminDB,
            false /* allowDeletions */,
            "InsufficientDiskSpace" /* reason */,
        );
        assert.commandFailedWithCode(
            testDB.runCommand({convertToCapped: "testColl", size: 100000}),
            ErrorCodes.ReplicaSetWritesBlocked,
        );
        assert(
            !testColl.stats().capped,
            "Collection should not have been converted while writes are blocked",
        );

        // Disable write block and check that convertToCapped succeeds.
        disableReplicaSetWriteBlock(
            this.replicaSetPrimaryAdminDB,
            "InsufficientDiskSpace" /* reason */,
        );
        assert.commandWorked(testDB.runCommand({convertToCapped: "testColl", size: 100000}));
        assert(
            testColl.stats().capped,
            "Collection should be capped after convertToCapped succeeds",
        );
    });
});
