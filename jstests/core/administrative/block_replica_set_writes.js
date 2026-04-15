// Test blockReplicaSetWrites command.
//
// @tags: [
//   requires_persistence,
//   requires_replication,
//   uses_parallel_shell,
//   does_not_support_stepdowns,
//   assumes_against_mongod_not_mongos,
//   assumes_read_concern_unchanged,
//   featureFlagBlockReplicaSetWrites
// ]

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {afterEach, before, describe, it} from "jstests/libs/mochalite.js";

describe("Test blockReplicaSetWrites command on replica set level", function () {
    before(function () {
        this.replicaSetPrimary = db.getMongo();
        // Extract numeric port for startParallelShell to connect directly to the replica set primary. When
        // using plain connections such as db.getMongo(), there is no metadata layer setting the port, so we
        // need to extract it manually.
        this.replicaSetPrimaryPort = parseInt(this.replicaSetPrimary.host.split(":")[1]);
        this.replicaSetPrimaryAdminDB = this.replicaSetPrimary.getDB("admin");
        this.replicaSetPrimaryConfigDB = this.replicaSetPrimary.getDB("config");
    });

    afterEach(function () {
        if (this.replicaSetPrimaryConfigDB.replica_set_writes_critical_section.findOne() === null) {
            return;
        }

        assert.commandWorked(
            this.replicaSetPrimaryAdminDB.runCommand({
                blockReplicaSetWrites: 1,
                enabled: false,
                allowDeletions: false,
                reason: "InsufficientDiskSpace",
            }),
        );
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

    it("Test that allowDeletions cannot be set to true when enabling replica set write block", function () {
        assert.commandFailedWithCode(
            this.replicaSetPrimaryAdminDB.runCommand({
                blockReplicaSetWrites: 1,
                enabled: true,
                allowDeletions: true,
                reason: "InsufficientDiskSpace",
            }),
            ErrorCodes.InvalidOptions,
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

        assert.commandWorked(
            this.replicaSetPrimaryAdminDB.runCommand({
                blockReplicaSetWrites: 1,
                enabled: false,
                allowDeletions: false,
                reason: "InsufficientDiskSpace",
            }),
        );

        const docsAfterRelease = this.replicaSetPrimaryConfigDB.replica_set_writes_critical_section.find().toArray();
        assert.eq(
            0,
            docsAfterRelease.length,
            "Expected no block replica set writes critical section documents after releasing critical section",
        );
    });
});
