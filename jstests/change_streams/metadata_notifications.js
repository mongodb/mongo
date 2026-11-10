// Tests of $changeStream notifications for metadata operations, written as independent test
// cases: each test sets up its own collection and captures its own resume tokens.
// Do not run in whole-cluster passthrough since one test assumes that the change stream will be
// invalidated by a database drop.
// @tags: [
//   do_not_run_in_whole_cluster_passthrough,
//   requires_fcv_63,
// ]
import {
    assertCreateCollection,
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ChangeStreamTest} from "jstests/libs/query/change_stream_util.js";

describe("change stream metadata notifications", () => {
    const testDB = db.getSiblingDB(jsTestName());
    const collName = "test";
    const renamedCollName = "renamed_coll";

    let cst;

    beforeEach(() => {
        cst = new ChangeStreamTest(testDB);
        assertDropAndRecreateCollection(testDB, collName);
        // Renames with dropTarget:false fail if the target exists. afterEach's dropDatabase
        // already guarantees renamedCollName is absent, but keep the precondition explicit here.
        assertDropCollection(testDB, renamedCollName);
    });

    afterEach(() => {
        cst.cleanUp();
        assert.commandWorked(testDB.dropDatabase());
    });

    // Opens a change stream on collName, inserts a document, drops the collection, and returns
    // the tokens of the events the stream observed:
    // {resumeToken, resumeTokenDrop, resumeTokenInvalidate}. The stream MUST be opened before
    // the insert: without a start option a stream begins one tick after the last applied
    // optime, so an earlier insert would not be delivered.
    function captureDropInvalidateTokens() {
        const cursor = cst.startWatchingChanges({
            collection: collName,
            pipeline: [{$changeStream: {}}],
        });
        const coll = testDB.getCollection(collName);
        assert.commandWorked(coll.insert({_id: 0}));
        assertDropCollection(testDB, collName);
        const changes = cst.assertNextChangesEqual({
            cursor: cursor,
            expectedChanges: [
                {
                    operationType: "insert",
                    ns: {db: testDB.getName(), coll: collName},
                    fullDocument: {_id: 0},
                    documentKey: {_id: 0},
                },
                {operationType: "drop", ns: {db: testDB.getName(), coll: collName}},
                {operationType: "invalidate"},
            ],
            expectInvalidate: true,
        });
        return {
            resumeToken: changes[0]._id,
            resumeTokenDrop: changes[1]._id,
            resumeTokenInvalidate: changes[2]._id,
        };
    }

    // $out cannot target a sharded collection (NamespaceCannotBeSharded), so the $out test
    // skips on sharded collections.
    function itWhenNotSharded(coll, name, block) {
        it(name, () => {
            if (FixtureHelpers.isSharded(testDB.getCollection(coll))) {
                jsTest.log.info("Skipping test: $out requires an unsharded collection", {name});
                return;
            }
            block();
        });
    }

    // Opens a change stream on collName, renames the collection to renamedCollName, and returns
    // the tokens of the rename and invalidate events the stream observed.
    function captureRenameInvalidateTokens({dropTarget = false} = {}) {
        const coll = testDB.getCollection(collName);
        const cursor = cst.startWatchingChanges({
            collection: collName,
            pipeline: [{$changeStream: {}}],
        });
        assert.commandWorked(coll.renameCollection(renamedCollName, dropTarget));
        const changes = cst.assertNextChangesEqual({
            cursor: cursor,
            expectedChanges: [
                {
                    operationType: "rename",
                    ns: {db: testDB.getName(), coll: collName},
                    to: {db: testDB.getName(), coll: renamedCollName},
                },
                {operationType: "invalidate"},
            ],
            expectInvalidate: true,
        });
        return {resumeTokenRename: changes[0]._id, resumeTokenInvalidate: changes[1]._id};
    }

    it("can open a collection-level change stream on a non-existent collection", () => {
        assertDropCollection(testDB, collName);
        const cursor = cst.startWatchingChanges({
            collection: collName,
            pipeline: [{$changeStream: {}}, {$project: {operationType: 1}}],
        });

        // Cursor creation succeeds, but there are no results. Guard the aggregate's firstBatch:
        // an unexpected event delivered there would otherwise be silently discarded by the
        // getMore that assertNoChange issues.
        assert.eq(cursor.firstBatch.length, 0, {firstBatch: cursor.firstBatch});

        // assertNoChange explicitly exercises getMore, to ensure that the getMore command for a
        // non-existent collection does not return an error, and that an empty batch does not
        // close the cursor.
        cst.assertNoChange(cursor);
        assert.neq(cursor.id, 0, {cursor});

        // Dropping the empty database should not generate any notification either, since the
        // collection does not exist yet.
        assert.commandWorked(testDB.dropDatabase());
        cst.assertNoChange(cursor);
        assert.neq(cursor.id, 0, {cursor});

        // After collection creation, we expect to see oplog entries for each subsequent
        // operation. The explicit creation is itself part of the check: it emits no event, so
        // the sentinel insert must be the FIRST event the stream delivers — if the drop had
        // generated a notification, it would have surfaced (and invalidated the stream) before
        // this insert.
        const coll = assertCreateCollection(testDB, collName);
        assert.commandWorked(coll.insert({_id: "sentinel"}));
        // The pipeline's $project reduces every event to its operationType.
        cst.assertNextChangesEqual({
            cursor: cursor,
            expectedChanges: [{operationType: "insert"}],
        });
    });

    it("reports CRUD events and a drop, then invalidates and closes the cursor", () => {
        const coll = testDB.getCollection(collName);
        // The $project reduces every event to its operationType: this test pins the event
        // SEQUENCE.
        const cursor = cst.startWatchingChanges({
            collection: collName,
            pipeline: [{$changeStream: {}}, {$project: {operationType: 1}}],
        });

        assert.commandWorked(coll.insert({_id: 1}));
        assert.commandWorked(coll.update({_id: 1}, {$set: {a: 1}}));
        assert.commandWorked(coll.remove({_id: 1}));
        assertDropCollection(testDB, collName);

        cst.assertNextChangesEqual({
            cursor: cursor,
            expectedChanges: [
                {operationType: "insert"},
                {operationType: "update"},
                {operationType: "delete"},
                {operationType: "drop"},
                {operationType: "invalidate"},
            ],
            expectInvalidate: true,
        });
    });

    it("startAfter an invalidate token opens a stream but yields no events", () => {
        // Exercises the bug described in SERVER-41196.
        const {resumeTokenInvalidate} = captureDropInvalidateTokens();
        const cursor = cst.startWatchingChanges({
            collection: collName,
            pipeline: [{$changeStream: {startAfter: resumeTokenInvalidate}}],
        });
        // The resumed stream must deliver nothing: guard the aggregate's firstBatch (an
        // unexpected event there would be silently discarded by the getMore that assertNoChange
        // issues), then verify via getMore that no events arrive and the cursor stays open.
        assert.eq(cursor.firstBatch.length, 0, {firstBatch: cursor.firstBatch});
        // The stream stays open and delivers nothing: the drop that caused the invalidate is
        // before the resume point, and the (still dropped) collection's re-appearance is what
        // the stream now waits for.
        cst.assertNoChange(cursor);
        assert.neq(cursor.id, 0, {cursor});
    });

    it("can resumeAfter an event token without an explicit collation", () => {
        const {resumeToken} = captureDropInvalidateTokens();
        const cursor = cst.startWatchingChanges({
            collection: collName,
            pipeline: [{$changeStream: {resumeAfter: resumeToken}}],
        });
        // Resuming from the insert token replays the drop (and, on sharded clusters, further
        // per-shard drops) before the invalidate. expectInvalidate must stay false here: with
        // it true, getOneChange asserts every event IS the invalidate and the drop-consuming
        // loop can never run.
        cst.consumeDropUpTo({
            cursor: cursor,
            dropType: "drop",
            expectedNext: {operationType: "invalidate"},
        });
        // The invalidate closes the cursor.
        assert.eq(cst.getNextBatch(cursor).id, 0);
    });

    it("resumeAfter the drop token with an explicit collation replays the drop and invalidate", () => {
        const {resumeTokenDrop} = captureDropInvalidateTokens();
        // Resumes open with an explicit simple collation; the no-collation case is covered
        // separately above.
        const cursor = cst.startWatchingChanges({
            collection: collName,
            pipeline: [{$changeStream: {resumeAfter: resumeTokenDrop}}],
            aggregateOptions: {collation: {locale: "simple"}, cursor: {batchSize: 0}},
        });
        // On a replica set, the only event after the drop entry is the synthesized invalidate.
        // On a sharded cluster, 'dropCollection' writes one drop entry per shard holding data
        // with distinct clusterTimes; resuming after the first delivered drop entry replays the
        // OTHER shards' drop entries before the invalidate. consumeDropUpTo consumes 0..n drop
        // events and asserts the next event is the invalidate. expectInvalidate stays false so
        // the drop-consuming loop can run (with it true, getOneChange asserts every event is
        // the invalidate).
        cst.consumeDropUpTo({
            cursor: cursor,
            dropType: "drop",
            expectedNext: {operationType: "invalidate"},
        });
        // The invalidate closes the cursor.
        assert.eq(cst.getNextBatch(cursor).id, 0);
    });

    it("resumeAfter the invalidate token is refused with InvalidResumeToken", () => {
        const {resumeTokenInvalidate} = captureDropInvalidateTokens();
        assert.commandFailedWithCode(
            testDB.runCommand({
                aggregate: collName,
                pipeline: [{$changeStream: {resumeAfter: resumeTokenInvalidate}}],
                cursor: {},
            }),
            ErrorCodes.InvalidResumeToken,
        );
    });

    it("holds the invalidate resume token even when the invalidate event is filtered out", () => {
        const {resumeToken, resumeTokenInvalidate} = captureDropInvalidateTokens();
        // Deliberately the native shell .watch() path: this test asserts on the shell cursor's
        // own getResumeToken(), not on tokens tracked by ChangeStreamTest.
        const resumeStream = testDB
            .getCollection(collName)
            .watch([{$match: {operationType: "DummyOperationType"}}], {resumeAfter: resumeToken});
        assert.soon(() => {
            assert(!resumeStream.hasNext());
            return resumeStream.isExhausted();
        });
        assert.eq(resumeStream.getResumeToken(), resumeTokenInvalidate);
    });

    it("startAfter the drop token replays the invalidate", () => {
        const {resumeTokenDrop} = captureDropInvalidateTokens();
        const cursor = cst.startWatchingChanges({
            collection: collName,
            pipeline: [{$changeStream: {startAfter: resumeTokenDrop}}],
            aggregateOptions: {collation: {locale: "simple"}},
        });
        cst.assertNextChangesEqual({
            cursor: cursor,
            expectedChanges: [{operationType: "invalidate"}],
            expectInvalidate: true,
        });
    });

    it("startAfter the invalidate token replays events that landed after the drop", () => {
        const {resumeTokenInvalidate} = captureDropInvalidateTokens();
        const coll = assertCreateCollection(testDB, collName);
        assert.commandWorked(coll.insert({_id: "after recreate"}));
        const cursor = cst.startWatchingChanges({
            collection: collName,
            pipeline: [{$changeStream: {startAfter: resumeTokenInvalidate}}],
            aggregateOptions: {collation: {locale: "simple"}},
        });
        cst.assertNextChangesEqual({
            cursor: cursor,
            expectedChanges: [
                {
                    operationType: "insert",
                    ns: {db: testDB.getName(), coll: collName},
                    fullDocument: {_id: "after recreate"},
                    documentKey: {_id: "after recreate"},
                },
            ],
        });
    });

    it("invalidates a change stream on the source of a rename", () => {
        // The helper asserts the rename + invalidate pair itself.
        captureRenameInvalidateTokens();
    });

    it("can resume from a rename invalidate by reopening the stream on the renamed collection", () => {
        const {resumeTokenInvalidate} = captureRenameInvalidateTokens();

        // The rename left the collection at renamedCollName. Reopening the stream with
        // 'startAfter' on the NEW namespace must work: 'startAfter' accepts
        // 'fromInvalidate' tokens, and the invalidate token retains the RENAME event's
        // collection UUID — which the rename preserves while changing the namespace.
        const renamedColl = testDB.getCollection(renamedCollName);
        assert.commandWorked(renamedColl.insert({_id: "after rename to renamed_coll"}));
        const cursor = cst.startWatchingChanges({
            collection: renamedCollName,
            pipeline: [{$changeStream: {startAfter: resumeTokenInvalidate}}],
            aggregateOptions: {collation: {locale: "simple"}},
        });
        cst.assertNextChangesEqual({
            cursor: cursor,
            expectedChanges: [
                {
                    operationType: "insert",
                    ns: {db: testDB.getName(), coll: renamedCollName},
                    fullDocument: {_id: "after rename to renamed_coll"},
                    documentKey: {_id: "after rename to renamed_coll"},
                },
            ],
        });
    });

    it("invalidates a change stream open on the target of a rename", () => {
        // collName is fresh from beforeEach, and renamedCollName is absent (afterEach drops the
        // database), so the collection can be renamed away directly.
        const coll = testDB.getCollection(collName);
        assert.commandWorked(coll.renameCollection(renamedCollName));

        // Watch the now non-existent original namespace; renaming the collection back onto it
        // invalidates the stream.
        const cursor = cst.startWatchingChanges({
            collection: collName,
            pipeline: [{$changeStream: {}}],
        });
        // RENAME #2: renamedCollName -> collName (direction reversed vs the setup rename).
        // This is the oplog entry the stream observes.
        assert.commandWorked(testDB.getCollection(renamedCollName).renameCollection(collName));
        cst.assertNextChangesEqual({
            cursor: cursor,
            expectedChanges: [
                {
                    operationType: "rename",
                    ns: {db: testDB.getName(), coll: renamedCollName},
                    to: {db: testDB.getName(), coll: collName},
                },
                {operationType: "invalidate"},
            ],
            expectInvalidate: true,
        });
    });

    it("resumeAfter the rename token replays the invalidate", () => {
        const {resumeTokenRename} = captureRenameInvalidateTokens();
        const cursor = cst.startWatchingChanges({
            collection: renamedCollName,
            pipeline: [{$changeStream: {resumeAfter: resumeTokenRename}}],
            aggregateOptions: {collation: {locale: "simple"}},
        });
        cst.assertNextChangesEqual({
            cursor: cursor,
            expectedChanges: [{operationType: "invalidate"}],
            expectInvalidate: true,
        });
    });

    it("resumeAfter the rename invalidate token is refused with InvalidResumeToken", () => {
        const {resumeTokenInvalidate} = captureRenameInvalidateTokens();
        assert.commandFailedWithCode(
            testDB.runCommand({
                aggregate: renamedCollName,
                pipeline: [{$changeStream: {resumeAfter: resumeTokenInvalidate}}],
                cursor: {},
                collation: {locale: "simple"},
            }),
            ErrorCodes.InvalidResumeToken,
        );
    });

    it("startAfter the rename token replays the invalidate", () => {
        const {resumeTokenRename} = captureRenameInvalidateTokens();
        const cursor = cst.startWatchingChanges({
            collection: renamedCollName,
            pipeline: [{$changeStream: {startAfter: resumeTokenRename}}],
            aggregateOptions: {collation: {locale: "simple"}},
        });
        cst.assertNextChangesEqual({
            cursor: cursor,
            expectedChanges: [{operationType: "invalidate"}],
            expectInvalidate: true,
        });
    });

    it("startAfter the rename invalidate token replays events that landed after the rename", () => {
        const {resumeTokenInvalidate} = captureRenameInvalidateTokens();
        const renamedColl = testDB.getCollection(renamedCollName);
        assert.commandWorked(renamedColl.insert({_id: "after rename"}));
        const cursor = cst.startWatchingChanges({
            collection: renamedCollName,
            pipeline: [{$changeStream: {startAfter: resumeTokenInvalidate}}],
            aggregateOptions: {collation: {locale: "simple"}},
        });
        cst.assertNextChangesEqual({
            cursor: cursor,
            expectedChanges: [
                {
                    operationType: "insert",
                    ns: {db: testDB.getName(), coll: renamedCollName},
                    fullDocument: {_id: "after rename"},
                    documentKey: {_id: "after rename"},
                },
            ],
        });
    });

    it("invalidates a change stream on the target of a dropTarget rename", () => {
        const coll = testDB.getCollection(collName);
        // The dropTarget rename replaces an existing target: create it (guaranteed absent by
        // beforeEach) and give it data.
        assertCreateCollection(testDB, renamedCollName);
        assert.commandWorked(testDB.renamed_coll.insert({_id: 0}));

        const cursor = cst.startWatchingChanges({
            collection: renamedCollName,
            pipeline: [{$changeStream: {}}],
        });
        assert.commandWorked(coll.renameCollection(renamedCollName, true /* dropTarget */));
        cst.assertNextChangesEqual({
            cursor: cursor,
            expectedChanges: [
                {
                    operationType: "rename",
                    ns: {db: testDB.getName(), coll: collName},
                    to: {db: testDB.getName(), coll: renamedCollName},
                },
                {operationType: "invalidate"},
            ],
            expectInvalidate: true,
        });
    });

    itWhenNotSharded(collName, "invalidates a change stream watching the target of a $out", () => {
        // The $out source must exist with data (guaranteed absent by beforeEach, so plain
        // create).
        const renamedColl = assertCreateCollection(testDB, renamedCollName);
        assert.commandWorked(renamedColl.insert({_id: 0}));
        assertDropCollection(testDB, collName);

        const cursor = cst.startWatchingChanges({
            collection: collName,
            pipeline: [{$changeStream: {}}],
        });
        renamedColl.aggregate([{$out: collName}]);
        // Note that $out will first create a temp collection, and then rename the temp collection
        // to the target. Do not explicitly check the 'ns' field.
        const rename = cst.getOneChange(cursor);
        assert.eq(rename.operationType, "rename", {rename});
        assert.eq(rename.to, {db: testDB.getName(), coll: collName}, {rename});
        assert.eq(cst.getOneChange(cursor, true).operationType, "invalidate");
    });

    it("invalidates collection-level streams when the database is dropped", () => {
        const cursor = cst.startWatchingChanges({
            collection: collName,
            pipeline: [{$changeStream: {}}],
        });
        assert.commandWorked(testDB.dropDatabase());
        cst.assertNextChangesEqual({
            cursor: cursor,
            expectedChanges: [
                {
                    operationType: "drop",
                    ns: {db: testDB.getName(), coll: collName},
                },
                {operationType: "invalidate"},
            ],
            expectInvalidate: true,
        });
    });
});
