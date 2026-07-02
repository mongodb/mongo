/**
 * Tests that the atClusterTime parameter in validate waits for the optime to be reached before
 * attempting to read at that timestamp.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

describe("validate atClusterTime on secondary waits for oplog application", function () {
    let rst, primary, secondary;

    before(function () {
        rst = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}]});
        rst.startSet();
        rst.initiate();
        primary = rst.getPrimary();
        secondary = rst.getSecondary();

        assert.commandWorked(primary.getDB("test").createCollection("coll"));
        assert.commandWorked(primary.getDB("test").coll.insertMany([{_id: 1}, {_id: 2}, {_id: 3}]));
        rst.awaitReplication();
    });

    after(function () {
        rst.stopSet();
    });

    it("blocks until secondary applies entries at atClusterTime", function () {
        // Pause the secondary's oplog applier at the top of its main loop, before it processes
        // any new batch. Using fp.wait() ensures the secondary is truly frozen before we insert
        // on the primary — so atClusterTime will always be ahead of the secondary's appliedOpTime.
        const pauseFp = configureFailPoint(secondary, "pauseOplogApplication");
        pauseFp.wait();

        try {
            // Insert with w:1. The secondary is frozen so w:2 would block indefinitely.
            const insertResult = assert.commandWorked(
                primary.getDB("test").runCommand({
                    insert: "coll",
                    documents: [{_id: 10}],
                    writeConcern: {w: 1},
                }),
            );
            const atClusterTime = insertResult.operationTime;

            // Confirm the secondary is behind.
            const statusBefore = assert.commandWorked(
                secondary.adminCommand({replSetGetStatus: 1}),
            );
            const selfBefore = statusBefore.members.find((m) => m.self);
            assert.lt(
                bsonWoCompare(selfBefore.optime.ts, atClusterTime),
                0,
                "secondary appliedOpTime should be behind atClusterTime",
                {appliedOpTime: selfBefore.optime.ts, atClusterTime},
            );

            // Validate blocks inside waitUntilOpTimeForRead() until the secondary's
            // appliedOpTime reaches atClusterTime. Since the secondary cannot make progress while
            // the failpoint is active, maxTimeMS causes MaxTimeMSExpired to be returned.
            secondary.getDB("test").setSecondaryOk();
            const validateResult = secondary.getDB("test").runCommand({
                validate: "coll",
                collHash: true,
                atClusterTime: atClusterTime,
                maxTimeMS: 500,
            });

            assert.commandFailedWithCode(
                validateResult,
                ErrorCodes.MaxTimeMSExpired,
                "expected validate to block waiting for secondary to apply up to atClusterTime",
                {validateResult, atClusterTime},
            );
        } finally {
            pauseFp.off();
        }

        // After oplog application resumes and the secondary catches up, validate should succeed.
        rst.awaitReplication();

        const insertResult2 = assert.commandWorked(
            primary.getDB("test").runCommand({
                insert: "coll",
                documents: [{_id: 11}],
                writeConcern: {w: 2},
            }),
        );
        const successResult = assert.commandWorked(
            secondary.getDB("test").runCommand({
                validate: "coll",
                collHash: true,
                atClusterTime: insertResult2.operationTime,
            }),
        );
        assert(
            successResult.valid,
            "validate should succeed once secondary has applied up to atClusterTime",
            {successResult},
        );
    });
});
