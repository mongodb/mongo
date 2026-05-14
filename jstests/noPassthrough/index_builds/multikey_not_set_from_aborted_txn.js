/**
 * Verifies that an index build does NOT mark a path multikey based on side-table writes that
 * originate from a transaction which is subsequently aborted.
 *
 * Repro shape:
 *   1. Insert a scalar document on a replica set with majority-commit enabled.
 *   2. Start (and pause) an index build on {a: 1, b: 1}. The collection is scalar-only, so the
 *      index should remain non-multikey unless a side-write claims otherwise.
 *   3. While the index build is paused (so the interceptor is the active writer path), open a
 *      transaction and insert a document whose field "a" is an array. The transactional write is
 *      buffered into the interceptor's side-writes table and (under the bug) immediately mutates
 *      the in-memory _multikeyPaths tracker.
 *   4. Abort the transaction. There is no RecoveryUnit::onRollback hook on the multikey tracker,
 *      so the tracker stays dirty even though the storage-engine write was rolled back.
 *   5. Resume and complete the index build. The interceptor flushes its multikey state into the
 *      durable catalog at commit-build time, poisoning the planner's view of the index.
 *
 * On patched code the assertion at the end of this test passes: the committed index is reported
 * non-multikey on field "a". On unpatched code it fails with isMultiKey == true, even though no
 * committed document in the collection makes the path multikey.
 *
 * @tags: [
 *   requires_replication,
 *   requires_majority_read_concern,
 *   uses_transactions,
 * ]
 */
import {getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            // Surface index-build logging if the test ever needs to be debugged in CI.
            logComponentVerbosity: tojson({index: 1, storage: {recovery: 1}}),
        },
    },
});
rst.startSet();
rst.initiate();

const dbName = jsTestName();
const collName = "coll";
const indexName = "a_1_b_1";

const primary = rst.getPrimary();
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB[collName];

// Seed the collection with strictly scalar documents. Nothing in the committed state should ever
// flip the index to multikey on field "a".
assert.commandWorked(primaryColl.insert([
    {_id: 0, a: 1, b: 10},
    {_id: 1, a: 2, b: 20},
    {_id: 2, a: 3, b: 30},
]));
rst.awaitReplication();

// Pause index builds so the side-writes interceptor is the active write path for any concurrent
// inserts. This is the regime in which the bug surfaces.
IndexBuildTest.pauseIndexBuilds(primary);

jsTestLog("Starting paused index build on {a: 1, b: 1}");
const awaitIndex = IndexBuildTest.startIndexBuild(
    primary,
    primaryColl.getFullName(),
    {a: 1, b: 1},
    {name: indexName},
);
IndexBuildTest.waitForIndexBuildToStart(primaryDB, collName, indexName);

jsTestLog("Opening transaction and inserting an array-valued document for field 'a'");
const session = primary.startSession();
const sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB.getCollection(collName);

session.startTransaction({writeConcern: {w: "majority"}});

// The array on field "a" is what would (incorrectly) cause the interceptor to mark the path
// multikey in the in-memory tracker. The matching _id is intentionally distinct from the seeded
// documents so the abort cleanly removes the only multikey-candidate document.
assert.commandWorked(sessionColl.insert({_id: 99, a: [100, 200, 300], b: 999}));

jsTestLog("Aborting transaction — multikey tracker must not retain the side-write's claim");
assert.commandWorked(session.abortTransaction_forTesting());
session.endSession();

// Sanity: the aborted document is not visible in the committed snapshot.
assert.eq(null, primaryColl.findOne({_id: 99}), "aborted document leaked into committed state");
assert.eq(3, primaryColl.find().itcount(), "unexpected committed document count after abort");

jsTestLog("Resuming index builds and waiting for completion");
IndexBuildTest.resumeIndexBuilds(primary);
awaitIndex();

// Confirm the index was actually built.
IndexBuildTest.assertIndexes(primaryColl, 2, ["_id_", indexName]);

// Validate at the storage level: collection should be valid and the index should not report any
// multikey paths for field "a".
const validateRes = assert.commandWorked(primaryColl.validate({full: true}));
assert(validateRes.valid, () => "collection validate reported invalid: " + tojson(validateRes));

// The planner-visible check: isMultiKey on the IXSCAN stage must be false.
const explain = primaryColl.find({a: 1}).hint(indexName).explain();
const plan = getWinningPlanFromExplain(explain);
assert.eq("FETCH", plan.stage, () => "unexpected winning-plan shape: " + tojson(explain));
assert.eq("IXSCAN", plan.inputStage.stage, () => "unexpected input stage: " + tojson(explain));
assert.eq(
    false,
    plan.inputStage.isMultiKey,
    () =>
        "index incorrectly reported multikey after aborted transaction; " +
        "multikey poison from side-write of aborted txn was not reverted. explain=" +
        tojson(explain),
);

// Also walk the multiKeyPaths map directly when present. Under the bug, "a" appears with a
// non-empty component-path set; under the fix, either the field is absent or the set is empty.
if (plan.inputStage.multiKeyPaths) {
    const aPaths = plan.inputStage.multiKeyPaths["a"];
    assert(
        aPaths === undefined || aPaths.length === 0,
        () => "field 'a' unexpectedly tracked as multikey: " + tojson(plan.inputStage.multiKeyPaths),
    );
    const bPaths = plan.inputStage.multiKeyPaths["b"];
    assert(
        bPaths === undefined || bPaths.length === 0,
        () => "field 'b' unexpectedly tracked as multikey: " + tojson(plan.inputStage.multiKeyPaths),
    );
}

// Catalog-level cross-check: collStats indexDetails should agree with the planner.
const stats = assert.commandWorked(primaryDB.runCommand({collStats: collName}));
if (stats.indexDetails && stats.indexDetails[indexName]) {
    const idxDetail = stats.indexDetails[indexName];
    if (idxDetail.hasOwnProperty("multikey")) {
        assert.eq(
            false,
            idxDetail.multikey,
            () => "collStats.indexDetails." + indexName + ".multikey was true after aborted txn",
        );
    }
}

rst.stopSet();
