/**
 * SERVER-122109: Selective PIT restore must filter post-checkpoint DDL ops.
 *
 * Selective point-in-time restore is supposed to restore only the namespaces the user
 * selected when the backup was taken. The backup flow:
 *   1. Take a consistent checkpoint at Tbackup.
 *   2. Copy data files for the selected namespaces (selective).
 *   3. Capture the oplog from Tbackup onward (non-selective).
 *
 * The restore flow:
 *   1. Restore the selected data files (state at Tbackup).
 *   2. Replay all captured oplog (non-selective), relying on NamespaceNotFound errors to
 *      silently no-op writes against unrestored namespaces.
 *
 * The bug: a `c` (command) oplog entry that *creates* a namespace will never hit
 * NamespaceNotFound — `createCollection` happily creates whatever it is told to create.
 * So any collection or view created *after* the checkpoint but before the restore point
 * gets resurrected in the restored cluster, even if it is outside the user's selection.
 *
 * This test pins that contract:
 *   - Build a one-node replica set.
 *   - Pre-create the "selected" collection so it appears in the checkpoint.
 *   - Take a checkpoint.
 *   - After the checkpoint, create both a selected ("keep") collection write and a
 *     non-selected ("drop") collection. The DDL ops for both land in the oplog after
 *     the checkpoint timestamp.
 *   - Restart the node in selective restore mode, applying the post-checkpoint oplog.
 *   - Assert: `keep.coll` exists with the expected docs AND `drop.coll` does NOT exist.
 *
 * Today this test is expected to fail at the final `drop.coll` assertion: the oplog
 * applier replays the `create` command non-selectively and the unwanted namespace
 * shows up in the restored catalog. Once the oplog filter is extended to DDL ops
 * (see src/mongo/db/repl/SERVER-122109-design.md), this test pins the fix.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 *   requires_majority_read_concern,
 *   # Selective restore is a single-node recovery flow.
 *   multiversion_incompatible,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

const keepDbName = "keep";
const dropDbName = "drop";
const collName = "coll";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let keepDb = primary.getDB(keepDbName);
let dropDb = primary.getDB(dropDbName);

// Pre-create the selected collection so the checkpoint contains its catalog entry and
// data files. The non-selected collection deliberately does NOT exist at checkpoint time
// — it will be born from a post-checkpoint `create` oplog entry, which is the case the
// bug ticket describes.
assert.commandWorked(keepDb.createCollection(collName));
assert.commandWorked(keepDb[collName].insert({_id: "pre-checkpoint", v: 1}));

// Force a checkpoint. From this moment forward, every operation lands in the oplog as a
// post-checkpoint entry that selective restore is supposed to filter against the
// user's namespace selection.
jsTestLog("Taking checkpoint at Tbackup");
assert.commandWorked(primary.adminCommand({fsync: 1}));
const checkpointTimestamp =
    assert.commandWorked(primary.adminCommand({hello: 1})).operationTime;
jsTestLog("Checkpoint timestamp: " + tojson(checkpointTimestamp));

// --- Post-checkpoint operations -------------------------------------------------------
// (A) A write to the selected collection. This MUST survive selective restore.
assert.commandWorked(keepDb[collName].insert({_id: "post-checkpoint-keep", v: 2}));

// (B) Create a brand-new, non-selected collection and write to it. The `create` and
//     `insert` entries both live in the oplog past `checkpointTimestamp`. The buggy
//     behaviour is that the `create` survives selective restore because the oplog
//     applier does not gate `c` ops against the user's namespace allowlist.
assert.commandWorked(dropDb.createCollection(collName));
assert.commandWorked(dropDb[collName].insert({_id: "post-checkpoint-drop", v: 99}));

// (C) Sanity: both collections are visible on the live primary before restore.
assert.eq(2, keepDb[collName].find().itcount(), "live keep.coll should have 2 docs");
assert.eq(1, dropDb[collName].find().itcount(), "live drop.coll should have 1 doc");

rst.awaitReplication();

// --- Selective restore ----------------------------------------------------------------
// The production selective-restore tooling (cloud backup agent) drops data files for
// non-selected namespaces between checkpoint copy and node restart, then runs recovery
// with --restore. We mirror that here by:
//   1. Stopping the node.
//   2. Dropping the non-selected namespace's catalog/data on disk by issuing a
//      drop *before* restart so the post-restart state mirrors a selective-data-file
//      restore where only the `keep.*` namespaces were copied.
//   3. Restarting with the `restore` flag so the oplog applier runs in selective-
//      restore mode and the NamespaceNotFound relaxation in oplog.cpp is active.
//
// The expectation of the fix:
//   - The `create` for `drop.coll` from the captured oplog must be SKIPPED because
//     `drop` is not in the restore-namespace allowlist.
//   - The `insert` into `drop.coll` then no-ops on NamespaceNotFound (existing
//     behaviour, already handled).

jsTestLog("Tearing down primary to simulate selective restore at restore time");
// Drop the non-selected database first so the on-disk state mirrors a selective copy
// where only `keep.*` files were restored from the checkpoint.
assert.commandWorked(dropDb.dropDatabase());
rst.awaitReplication();

const restartOpts = {
    setParameter: {
        // Selective restore mode: enables the `storageGlobalParams.restore` relaxation
        // path in src/mongo/db/repl/oplog.cpp that tolerates NamespaceNotFound during
        // oplog replay against unrestored collections. After SERVER-122109 this same
        // mode will additionally filter DDL `c` ops against the restore-namespace
        // allowlist.
        "restore": true,
    },
};

jsTestLog("Restarting node in selective restore mode");
primary = rst.restart(0, restartOpts);
rst.awaitSecondaryNodes();
primary = rst.getPrimary();
keepDb = primary.getDB(keepDbName);
dropDb = primary.getDB(dropDbName);

// --- Assertions -----------------------------------------------------------------------
jsTestLog("Verifying selected collection survives and contains both pre- and post-" +
          "checkpoint documents");
const keepDocs = keepDb[collName].find().sort({_id: 1}).toArray();
assert.eq(2, keepDocs.length, "keep.coll should have both pre- and post-checkpoint docs");
assert.eq("post-checkpoint-keep", keepDocs[0]._id);
assert.eq("pre-checkpoint", keepDocs[1]._id);

jsTestLog("Verifying non-selected collection was NOT resurrected by oplog replay");
// The key assertion. Today this fails: the post-checkpoint `create drop.coll` oplog
// entry is replayed without filtering and `drop.coll` reappears in the catalog with
// the post-checkpoint `insert` applied on top of it.
const dropCollections =
    dropDb.getCollectionInfos({name: collName});
assert.eq(0,
          dropCollections.length,
          "drop.coll must NOT exist after selective restore — found: " +
              tojson(dropCollections));

// And no document should be readable from it either.
assert.eq(0,
          dropDb[collName].find().itcount(),
          "drop.coll must have zero documents after selective restore");

rst.stopSet();
