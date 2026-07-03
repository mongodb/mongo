/**
 * SERVER-101428 regression: a cross-database renameCollection must hold the source collection lock
 * in MODE_X for the entire operation (data copy + final rename/drop). Previously the source and
 * temporary collection locks were released between the copy and the final rename, allowing a
 * concurrent write to the source to be executed in between.
 *
 * This test deterministically pauses the rename in that former lock-drop window (via the
 * 'hangRenameCollectionAcrossDatabasesBeforeFinalize' failpoint) and asserts that a concurrent
 * write to the source collection blocks on the lock (it times out) rather than succeeding. Once
 * the rename completes the data must be intact under the target namespace.
 *
 * @tags: [
 *   multiversion_incompatible,
 *   requires_replication,
 *   uses_rename,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

describe("cross-database renameCollection lock holding", function () {
    const kSourceDbName = "rename_src_db";
    const kTargetDbName = "rename_tgt_db";
    const kCollName = "coll";
    const sourceNss = `${kSourceDbName}.${kCollName}`;
    const targetNss = `${kTargetDbName}.${kCollName}`;

    before(function () {
        this.rst = new ReplSetTest({nodes: 1});
        this.rst.startSet();
        this.rst.initiate();
        this.primary = this.rst.getPrimary();
    });

    after(function () {
        this.rst.stopSet();
    });

    it("holds the source lock across the copy/finalize window", function () {
        const primary = this.primary;
        const sourceDb = primary.getDB(kSourceDbName);

        assert.commandWorked(sourceDb.createCollection(kCollName));
        assert.commandWorked(sourceDb[kCollName].insert({_id: 0, n: 0}));

        // Pause the rename after the data copy but before the final rename/drop.
        const fp = configureFailPoint(primary, "hangRenameCollectionAcrossDatabasesBeforeFinalize");

        const renameShell = startParallelShell(
            funWithArgs(
                function (sourceNss, targetNss) {
                    assert.commandWorked(
                        db.adminCommand({
                            renameCollection: sourceNss,
                            to: targetNss,
                            dropTarget: true,
                        }),
                    );
                },
                sourceNss,
                targetNss,
            ),
            primary.port,
        );

        // Wait until the rename is parked in the former lock-drop window, still holding the
        // source lock.
        fp.wait();

        // A write to the source collection must block on the source MODE_X lock. With a short
        // maxTimeMS it must time out; before the fix the lock was released here and the write
        // would have succeeded.
        const writeRes = sourceDb.runCommand({
            findAndModify: kCollName,
            query: {_id: 0},
            update: {$inc: {n: 1}},
            maxTimeMS: 5 * 1000,
        });
        assert.commandFailedWithCode(
            writeRes,
            [ErrorCodes.MaxTimeMSExpired, ErrorCodes.LockTimeout],
            "write to source should have blocked on the held source lock",
        );

        // Let the rename finish.
        fp.off();
        renameShell();

        // The renamed data must be intact under the target namespace, and the source gone.
        const targetDb = primary.getDB(kTargetDbName);
        assert.eq(
            0,
            sourceDb[kCollName].countDocuments({}),
            "source collection should no longer exist",
        );
        assert.eq(
            [{_id: 0, n: 0}],
            targetDb[kCollName].find().toArray(),
            "target data must be intact",
        );
    });
});
