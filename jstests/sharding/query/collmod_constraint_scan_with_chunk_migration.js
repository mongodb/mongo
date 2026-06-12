/**
 * Targeted tests for constraint validation scan behavior with concurrent chunk migrations.
 *
 * Scenario 1: A chunk migration moves an invalid document after the scan cursor has been established.
 * Range preservers on the donor shard must keep the document visible to the in-progress cursor so
 * the scan still detects it even after the chunk has been committed to the destination shard.
 *
 * Scenario 2: An invalid document is concurrently updated to become valid while the scan cursor
 * is paused. The orphan on the donor shard still carries the stale invalid value, so the scan
 * detects it and collMod fails even though the primary copy is already valid. This is an
 * acceptable false positive. Then retry the collMod to demonstrate it was a false positive.
 *
 * @tags: [
 *   requires_fcv_90,
 *   uses_parallel_shell,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {after, afterEach, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("constraint validation scan with concurrent chunk migrations", function () {
    let st;
    let disableOpt;
    let hangCursor;
    let awaitCollMod;
    const dbName = jsTestName();
    const collName = "test";
    const ns = dbName + "." + collName;
    const validator = {_id: {$exists: true}, a: {$exists: true}};

    before(function () {
        st = new ShardingTest({shards: 2});
        // Pin the coordinator to shard0 by making it the primary shard.
        assert.commandWorked(
            st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
        );
    });

    after(function () {
        st.stop();
    });

    afterEach(function () {
        hangCursor?.off();
        hangCursor = null;
        awaitCollMod?.();
        awaitCollMod = null;
        disableOpt?.off();
        disableOpt = null;
    });

    function setupCollection() {
        const testDb = st.s.getDB(dbName);
        testDb[collName].drop();
        assert.commandWorked(
            testDb.createCollection(collName, {
                validator: validator,
                validationLevel: "strict",
                validationAction: "warn",
            }),
        );
        // 10 documents on shard1 (_id 0-9). The invalid doc is at _id 7.
        for (let i = 0; i < 10; i++) {
            assert.commandWorked(
                testDb[collName].insert(i === 7 ? {_id: 7, b: 1} : {_id: i, a: 1}),
            );
        }
        assert.commandWorked(
            testDb.runCommand({collMod: collName, prepareConstraintValidationLevel: true}),
        );
        // Split: _id < 0 stays on shard0 (coordinator), _id >= 0 goes to shard1.
        st.shardColl(collName, {_id: 1}, {_id: 0}, {_id: 1}, dbName, true);
    }

    it("range preservers keep an invalid document visible when its chunk migrates mid-scan", function () {
        setupCollection();
        const testDb = st.s.getDB(dbName);

        // Turn off pipeline optimization so that we hit the agg failpoint.
        const shard1Primary = st.rs1.getPrimary();
        disableOpt = configureFailPoint(shard1Primary, "disablePipelineOptimization");
        // Pause before the aggregation cursor loads its first batch on shard1. The underlying
        // plan executor snapshot is already established, so the invalid doc at _id 7 is visible.
        hangCursor = configureFailPoint(shard1Primary, "hangBeforeDocumentSourceCursorLoadBatch", {
            nss: ns,
        });

        // Start the collMod in a parallel shell. The scan will fan out to shard1 and pause
        // before processing any documents.
        awaitCollMod = startParallelShell(
            funWithArgs(
                (dbName, collName) => {
                    // The range preserver must keep the orphaned invalid doc visible, so the
                    // scan must fail even though the chunk is no longer owned by this shard.
                    assert.commandFailedWithCode(
                        db.getSiblingDB(dbName).runCommand({
                            collMod: collName,
                            validationLevel: "constraint",
                            validationAction: "error",
                        }),
                        12370902,
                    );
                },
                dbName,
                collName,
            ),
            st.s.port,
        );

        // Wait until the scan has actually paused on shard1.
        hangCursor.wait();

        // Migrate the chunk while the scan cursor is open on shard1. The migration commits
        // without waiting for range deletion (_waitForDelete omitted) because the open scan
        // cursor holds a range preserver that blocks orphan cleanup on shard1 until the cursor
        // closes. The scan will still see the invalid doc as an orphan.
        assert.commandWorked(
            st.s.adminCommand({
                moveChunk: ns,
                bounds: [{_id: 0}, {_id: MaxKey}],
                to: st.shard0.shardName,
            }),
        );

        // Assert doc on both shards.
        // Use findOne (a find command, not aggregation) to avoid triggering the agg failpoint.
        assert.neq(null, st.rs0.getPrimary().getDB(dbName)[collName].findOne({_id: 7}));
        assert.neq(null, st.rs1.getPrimary().getDB(dbName)[collName].findOne({_id: 7}));
        hangCursor.off();
        awaitCollMod();

        // Wait for the range deletion task on shard1 to clean up the orphan now that the
        // cursor has closed and the range preserver has been released.
        assert.soon(
            () => st.rs1.getPrimary().getDB(dbName)[collName].countDocuments({_id: 7}) === 0,
            "orphan on shard1 should be cleaned up after the range preserver is released",
        );
    });

    it("scan reports invalid document as a false positive when it is concurrently updated to valid", function () {
        setupCollection();
        const testDb = st.s.getDB(dbName);

        // Turn off pipeline optimization so that we hit the agg failpoint.
        const shard1Primary = st.rs1.getPrimary();
        disableOpt = configureFailPoint(shard1Primary, "disablePipelineOptimization");
        // Pause before the aggregation cursor loads its first batch on shard1. The cursor
        // snapshot is already established at T0 when shard1 owns [0, MaxKey).
        hangCursor = configureFailPoint(shard1Primary, "hangBeforeDocumentSourceCursorLoadBatch", {
            nss: ns,
        });

        // Start the collMod in a parallel shell. The cursor on shard1 was established at T0
        // when shard1 owned [0, MaxKey), so the ShardFilterStage will still allow the orphaned
        // invalid doc once the migration commits. The scan therefore fails even though the
        // primary copy on shard0 will be updated to valid before the scan resumes.
        awaitCollMod = startParallelShell(
            funWithArgs(
                (dbName, collName) => {
                    assert.commandFailedWithCode(
                        db.getSiblingDB(dbName).runCommand({
                            collMod: collName,
                            validationLevel: "constraint",
                            validationAction: "error",
                        }),
                        12370902,
                    );
                },
                dbName,
                collName,
            ),
            st.s.port,
        );

        hangCursor.wait();

        // Migrate the chunk without _waitForDelete: the orphan stays on shard1 until the
        // scan cursor closes and the range preserver is released.
        assert.commandWorked(
            st.s.adminCommand({
                moveChunk: ns,
                bounds: [{_id: 0}, {_id: MaxKey}],
                to: st.shard0.shardName,
            }),
        );

        // Assert doc on both shards.
        // Use findOne (a find command, not aggregation) to avoid triggering the agg failpoint.
        assert.neq(null, st.rs0.getPrimary().getDB(dbName)[collName].findOne({_id: 7}));
        assert.neq(null, st.rs1.getPrimary().getDB(dbName)[collName].findOne({_id: 7}));

        // Update the document to valid on shard0 (the new primary). The orphan on shard1
        // still carries the old invalid value — the update does not reach orphaned copies.
        assert.commandWorked(testDb[collName].updateOne({_id: 7}, {$set: {a: 1}}));
        hangCursor.off();
        awaitCollMod();

        // Wait for the range deletion task on shard1 to clean up the orphan now that the
        // cursor has closed and the range preserver has been released.
        assert.soon(
            () => st.rs1.getPrimary().getDB(dbName)[collName].countDocuments({_id: 7}) === 0,
            "orphan on shard1 should be cleaned up after the range preserver is released",
        );

        // Should succeed now that the orphan is gone and the primary copy is already valid.
        assert.commandWorked(
            testDb.runCommand({
                collMod: collName,
                validationLevel: "constraint",
                validationAction: "error",
            }),
        );
    });
});
