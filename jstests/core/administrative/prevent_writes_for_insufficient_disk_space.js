// Test preventWritesForInsufficientDiskSpace command.
//
// @tags: [
//   requires_persistence,
//   requires_replication,
//   uses_parallel_shell,
//   does_not_support_stepdowns,
//   assumes_against_mongod_not_mongos,
//   assumes_read_concern_unchanged,
//   featureFlagPreventWritesForInsufficientDiskSpace
// ]

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {afterEach, before, describe, it} from "jstests/libs/mochalite.js";

describe("Test preventWritesForInsufficientDiskSpace command on shard node", function () {
    before(function () {
        this.shardPrimary = db.getMongo();
        // Extract numeric port for startParallelShell to connect directly to the shard primary. When using
        // plain connections such as db.getMongo(), there is no metadata layer setting the port, so we need
        // to extract it manually.
        this.shardPrimaryPort = parseInt(this.shardPrimary.host.split(":")[1]);
        this.shardPrimaryAdminDB = this.shardPrimary.getDB("admin");
        this.shardPrimaryConfigDB = this.shardPrimary.getDB("config");
    });

    afterEach(function () {
        if (this.shardPrimaryConfigDB.prevent_writes_critical_sections.findOne() === null) {
            return;
        }

        assert.commandWorked(
            this.shardPrimaryAdminDB.runCommand({
                preventWritesForInsufficientDiskSpace: 1,
                enabled: false,
                allowDeletions: false,
                reason: "InsufficientDiskSpace",
            }),
        );
    });

    it("Test that concurrent preventWritesForInsufficientDiskSpace commands serialize", function () {
        const hangFailPoint = configureFailPoint(
            this.shardPrimary,
            "hangInPreventWritesForInsufficientDiskSpaceCommand",
        );

        jsTest.log.info("Starting parallel shell to run parallel preventWritesForInsufficientDiskSpace command");
        const awaitShell = startParallelShell(() => {
            assert.commandWorked(
                db.getSiblingDB("admin").runCommand({
                    preventWritesForInsufficientDiskSpace: 1,
                    enabled: true,
                    allowDeletions: false,
                    reason: "InsufficientDiskSpace",
                }),
            );
        }, this.shardPrimaryPort);

        jsTest.log.info("Wait for fail point to be hit");
        hangFailPoint.wait();

        jsTest.log.info(
            "Try to run a second preventWritesForInsufficientDiskSpace command, which should timeout since the first command is still running",
        );
        assert.commandFailedWithCode(
            this.shardPrimaryAdminDB.runCommand({
                preventWritesForInsufficientDiskSpace: 1,
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

    it("Test that allowDeletions cannot be set to true when enabling writes block", function () {
        assert.commandFailedWithCode(
            this.shardPrimaryAdminDB.runCommand({
                preventWritesForInsufficientDiskSpace: 1,
                enabled: true,
                allowDeletions: true,
                reason: "InsufficientDiskSpace",
            }),
            ErrorCodes.InvalidOptions,
        );
    });

    it("Test that the prevent-writes critical section document is persisted correctly", function () {
        assert.commandWorked(
            this.shardPrimaryAdminDB.runCommand({
                preventWritesForInsufficientDiskSpace: 1,
                enabled: true,
                allowDeletions: false,
                reason: "InsufficientDiskSpace",
            }),
        );

        const doc = this.shardPrimaryConfigDB.prevent_writes_critical_sections.findOne();
        assert.eq(true, doc.enabled, "Expected enabled to be true");
        assert.eq(false, doc.allowDeletions, "Expected allowDeletions to be false");
        assert.eq(
            "InsufficientDiskSpace",
            doc.preventWritesReason,
            "Expected preventWritesReason to be InsufficientDiskSpace",
        );

        assert.commandWorked(
            this.shardPrimaryAdminDB.runCommand({
                preventWritesForInsufficientDiskSpace: 1,
                enabled: false,
                allowDeletions: false,
                reason: "InsufficientDiskSpace",
            }),
        );

        const docsAfterRelease = this.shardPrimaryConfigDB.prevent_writes_critical_sections.find().toArray();
        assert.eq(
            0,
            docsAfterRelease.length,
            "Expected no prevent-writes critical section documents after releasing critical section",
        );
    });
});
