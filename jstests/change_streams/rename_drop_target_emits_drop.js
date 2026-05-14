/**
 * Regression test for SERVER-53478: when `renameCollection` is invoked with `dropTarget: true`,
 * the target collection is implicitly dropped to make room for the rename. A change stream open
 * on the (about-to-be-overwritten) target collection currently receives nothing for that drop --
 * it sees no `drop` event and no `invalidate`, which means any consumer that relies on `drop`
 * events to invalidate downstream state silently skips the overwritten collection.
 *
 * Desired behaviour (per SERVER-53478 / SERVER-98496):
 *   1. A change stream open on the *original* target collection receives a synthetic `drop` event
 *      (followed by `invalidate`) when that collection is overwritten by `renameCollection
 *      ..., {dropTarget: true}`.
 *   2. A whole-database change stream observes both a `drop` event (for the overwritten target)
 *      AND a `rename` event (for src -> dst), in that order, in the same oplog cluster time.
 *
 * Before the fix lands this test is expected to FAIL on the `drop` assertion in case (1) and on
 * the ordered-events assertion in case (2). After the fix, all assertions hold.
 *
 * Do not run in whole-cluster passthroughs: a db-scoped change stream is part of the contract.
 * Sharded collections cannot be renamed, so skip on sharded fixtures.
 *
 * @tags: [
 *   do_not_run_in_whole_cluster_passthrough,
 *   requires_fcv_63,
 *   assumes_unsharded_collection,
 * ]
 */
import {
    assertCreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {ChangeStreamTest} from "jstests/libs/query/change_stream_util.js";

const testDb = db.getSiblingDB(jsTestName());

// Sharded collections cannot be renamed; skip on sharded fixtures.
if (FixtureHelpers.isSharded(testDb["__probe"])) {
    jsTestLog("Skipping SERVER-53478 regression test on sharded fixture");
    quit();
}

const srcCollName = "rename_src";
const dstCollName = "rename_dst";

// Reset state from any previous run.
assertDropCollection(testDb, srcCollName);
assertDropCollection(testDb, dstCollName);

// Seed both collections so the rename has a real target to overwrite.
const srcColl = assertCreateCollection(testDb, srcCollName);
const dstColl = assertCreateCollection(testDb, dstCollName);
assert.commandWorked(srcColl.insert({_id: "src-doc"}));
assert.commandWorked(dstColl.insert({_id: "dst-doc-to-be-dropped"}));

// ---------------------------------------------------------------------------
// Case 1: change stream open on the *target* collection sees the synthetic
// drop + invalidate when the target is overwritten by `dropTarget: true`.
// ---------------------------------------------------------------------------
{
    const cst = new ChangeStreamTest(testDb);
    const cursor = cst.startWatchingChanges({
        collection: dstCollName,
        pipeline: [{$changeStream: {}}],
    });

    // Trigger the rename that drops the existing target.
    assert.commandWorked(srcColl.renameCollection(dstCollName, true /* dropTarget */));

    // We expect: drop (synthetic, for the overwritten dst) -> invalidate.
    // Before SERVER-53478 is fixed this assertion fails: the watcher sees nothing.
    const expectedChanges = [
        {
            operationType: "drop",
            ns: {db: testDb.getName(), coll: dstCollName},
        },
        {operationType: "invalidate"},
    ];
    cst.assertNextChangesEqual({
        cursor: cursor,
        expectedChanges: expectedChanges,
        expectInvalidate: true,
    });

    cst.cleanUp();
}

// ---------------------------------------------------------------------------
// Case 2: db-scoped change stream observes both `drop` (for the overwritten
// target) AND `rename` (for src -> dst) from the same rename command, with
// `drop` ordered before `rename` so downstream consumers see the
// invalidation of the old target before the rename of the new occupant.
// ---------------------------------------------------------------------------
{
    // Reset.
    assertDropCollection(testDb, srcCollName);
    assertDropCollection(testDb, dstCollName);
    const srcCollV2 = assertCreateCollection(testDb, srcCollName);
    const dstCollV2 = assertCreateCollection(testDb, dstCollName);
    assert.commandWorked(srcCollV2.insert({_id: "src-doc-v2"}));
    assert.commandWorked(dstCollV2.insert({_id: "dst-doc-v2-to-be-dropped"}));

    const cst = new ChangeStreamTest(testDb);
    const cursor = cst.startWatchingChanges({
        collection: 1,  // db-scoped change stream
        pipeline: [
            {$changeStream: {showExpandedEvents: true}},
            {$match: {operationType: {$in: ["drop", "rename"]}}},
        ],
    });

    assert.commandWorked(srcCollV2.renameCollection(dstCollName, true /* dropTarget */));

    const expectedChanges = [
        {
            operationType: "drop",
            ns: {db: testDb.getName(), coll: dstCollName},
        },
        {
            operationType: "rename",
            ns: {db: testDb.getName(), coll: srcCollName},
            to: {db: testDb.getName(), coll: dstCollName},
        },
    ];
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: expectedChanges});

    cst.cleanUp();
}

// ---------------------------------------------------------------------------
// Case 3 (negative): dropTarget:true with NO existing target must NOT emit a
// synthetic drop. The synthetic event is gated on the oplog `o.dropTarget`
// UUID actually being present, which the server only writes when an existing
// collection was overwritten.
// ---------------------------------------------------------------------------
{
    assertDropCollection(testDb, srcCollName);
    assertDropCollection(testDb, dstCollName);
    const srcCollV3 = assertCreateCollection(testDb, srcCollName);
    assert.commandWorked(srcCollV3.insert({_id: "src-doc-v3"}));

    const cst = new ChangeStreamTest(testDb);
    const cursor = cst.startWatchingChanges({
        collection: 1,
        pipeline: [
            {$changeStream: {}},
            {$match: {operationType: {$in: ["drop", "rename"]}}},
        ],
    });

    // dropTarget:true but no existing target: this is a plain rename. The server
    // omits `o.dropTarget` from the oplog entry, so no synthetic drop should be
    // emitted.
    assert.commandWorked(srcCollV3.renameCollection(dstCollName, true /* dropTarget */));

    const expectedChanges = [
        {
            operationType: "rename",
            ns: {db: testDb.getName(), coll: srcCollName},
            to: {db: testDb.getName(), coll: dstCollName},
        },
    ];
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: expectedChanges});

    cst.cleanUp();
}
