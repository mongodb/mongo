/**
 * Tests that a change stream can resume correctly after rename invalidation.
 *
 * @tags: [
 *   assumes_balancer_off,
 *   does_not_support_stepdowns,
 *   featureFlagChangeStreamPreciseShardTargeting,
 *   requires_sharding,
 *   uses_change_streams,
 * ]
 */
import {describe, it, before, afterEach, after} from "jstests/libs/mochalite.js";
import {
    assertOpenCursors,
    ChangeStreamTest,
    ChangeStreamWatchMode,
    cursorCommentFilter,
    waitForClusterTime,
} from "jstests/libs/query/change_stream_util.js";
import {ChangeStreamReader, ChangeStreamReadingMode} from "jstests/libs/util/change_stream/change_stream_reader.js";
import {Writer} from "jstests/libs/util/change_stream/change_stream_writer.js";
import {Connector} from "jstests/libs/util/change_stream/change_stream_connector.js";
import {
    CreateDatabaseCommand,
    CreateUnsplittableCollectionCommand,
    DropCollectionCommand,
    InsertDocCommand,
    RenameToNonExistentSameDbCommand,
} from "jstests/libs/util/change_stream/change_stream_commands.js";
import {
    buildExpectedEvents,
    createMatcher,
    createShardingTest,
    runTeardownSteps,
} from "jstests/libs/util/change_stream/change_stream_sharding_utils.js";
import {
    VerifierContext,
    SingleReaderVerificationTestCase,
} from "jstests/libs/util/change_stream/change_stream_verifier.js";

describe("$changeStream", function () {
    let st, shards;
    const testDb = jsTestName();
    const testColl = "test_coll";

    before(function () {
        st = createShardingTest(/*mongos*/ 1, /*shards*/ 2);
        shards = assert.commandWorked(st.s.adminCommand({listShards: 1})).shards;

        Random.setRandomSeed();
    });

    afterEach(function () {
        runTeardownSteps(
            () => Writer.joinAll(),
            () => ChangeStreamReader.joinAll(),
            () => st.s.getDB(testDb).dropDatabase(),
            () => st.s.getDB(Connector.controlDatabase).dropDatabase(),
        );
    });

    after(function () {
        st.stop();
    });

    it("resumes after rename invalidate on non-primary shard", function () {
        // Scenario:
        //  - DB primary pinned to shard0
        //  - Collection created on shard1 (non-primary) as unsplittable
        //  - Collection renamed away (invalidate)
        //  - Original name recreated by insert (lands on shard0)
        //
        // Expected events: [create, rename, invalidate, create, insert]
        // Pre-create the database (with primary on shard0) outside the writer so that the
        // target-collection stream below can open with deterministic placement (cursor on the
        // DB primary). enableSharding is not visible to change streams.
        new CreateDatabaseCommand({dbName: testDb, primaryShard: shards[0]._id}).execute(st.s);

        const commands = [
            new CreateUnsplittableCollectionCommand({
                dbName: testDb,
                collName: testColl,
                shardSet: [shards[1]],
                collectionCtx: {exists: false, isSharded: false},
            }),
            new RenameToNonExistentSameDbCommand({
                dbName: testDb,
                collName: testColl,
                shardSet: shards,
                collectionCtx: {exists: true, isSharded: false},
                // Keep testColl_renamed alive after the rename so a follow-up watch
                // can observe the v2 targeter placing its cursor on shard1. The
                // invalidate is triggered by the rename itself (rename-to-watched-
                // namespace), not by the manual drop done in afterEach cleanup.
                dropAfterRename: false,
            }),
            new InsertDocCommand({
                dbName: testDb,
                collName: testColl,
                collectionCtx: {exists: false, isSharded: false},
            }),
        ];
        const expectedEvents = buildExpectedEvents(commands, ChangeStreamWatchMode.kCollection);
        // Wait for cluster time on the config server to advance to a point so that a change
        // stream opened with startAtOperationTime = startTime is not considered a future
        // cluster time.
        const startTime = waitForClusterTime(st.s.getDB("admin"), st);

        // Open a change stream on the rename target collection BEFORE running the writer to
        // observe v2 targeter retargeting from the DB primary to the non-primary shard.
        const targetColl = `${testColl}_renamed`; // matches RenameToNonExistentSameDbCommand
        const comment = "rename_target_retargeting";
        const csTest = new ChangeStreamTest(st.s.getDB(testDb));
        const targetCursor = csTest.startWatchingChanges({
            pipeline: [{$changeStream: {version: "v2", startAtOperationTime: startTime}}],
            collection: targetColl,
            aggregateOptions: {comment, cursor: {batchSize: 1}},
        });

        // Initial placement: the target collection does not yet exist, but the DB is present
        // with primary on shard0. The v2 targeter falls back to the DB primary, so a single
        // cursor is opened on shard0 and no config-server placement watcher is needed.
        assertOpenCursors(st, [shards[0]._id], /*expectedConfigCursor=*/ false, cursorCommentFilter(comment));

        // Execute commands via Writer.
        const writerName = "writer_rename_resume";
        Writer.run(st.s, writerName, commands);

        // Read events via ChangeStreamReader (handles invalidate + startAfter resume).
        const readerName = "reader_rename_resume";
        const readerConfig = {
            dbName: testDb,
            collName: testColl,
            watchMode: ChangeStreamWatchMode.kCollection,
            readingMode: ChangeStreamReadingMode.kFetchOneAndResume,
            startAtClusterTime: startTime,
            numberOfEventsToRead: expectedEvents.length,
            instanceName: readerName,
        };
        ChangeStreamReader.run(st.s, readerConfig);

        // Build VerifierContext and run SingleReaderVerificationTestCase.
        const ctx = new VerifierContext({[readerName]: readerConfig}, {[readerName]: createMatcher(expectedEvents)});
        new SingleReaderVerificationTestCase(readerName).run(st.s, ctx);

        // Wait for the writer so all events (including the trailing drop) have flushed.
        Connector.waitForDone(st.s, writerName);
        Writer.joinAll();

        // Sanity check: with dropAfterRename: false, the rename target should still exist
        // on shard1 after the writer finishes. If this fails, an unexpected drop happened.
        const collsOnTargetDb = st.s.getDB(testDb).getCollectionNames();
        assert(
            collsOnTargetDb.includes(targetColl),
            `Expected ${targetColl} to still exist after writer; found collections: ${tojsononeline(collsOnTargetDb)}`,
        );

        // A rename TO the watched namespace invalidates a collection-level change stream:
        // the cursor delivers the rename event and then an immediate `invalidate`.
        // See https://www.mongodb.com/docs/manual/reference/method/db.collection.renameCollection/
        // Receiving the rename here also implicitly proves the targeter retargeted from
        // shard0 to shard1.
        csTest.assertNextChangesEqual({
            cursor: targetCursor,
            expectedChanges: [
                {
                    operationType: "rename",
                    ns: {db: testDb, coll: testColl},
                    to: {db: testDb, coll: targetColl},
                },
                {operationType: "invalidate"},
            ],
            expectInvalidate: true,
        });

        // Verify the v2 targeter opens a cursor on shard1 only for testColl_renamed.
        // waitForClusterTime ensures initialization completes on the first getMore without
        // entering the Waiting state, so no log-based assertion is needed.
        const verifyTime = waitForClusterTime(st.s.getDB("admin"), st);
        const verifyCursor = csTest.startWatchingChanges({
            pipeline: [{$changeStream: {version: "v2", startAtOperationTime: verifyTime}}],
            collection: targetColl,
            aggregateOptions: {cursor: {batchSize: 0}},
        });
        csTest.assertNoChange(verifyCursor);
        // Confirm the cursor landed on shard1 only.
        assertOpenCursors(st, [shards[1]._id], /*expectedConfigCursor=*/ false, {ns: `${testDb}.${targetColl}`});

        // Clean up the lingering testColl_renamed so afterEach's dropDatabase doesn't trip
        // on a surprise collection (the rename command no longer drops it for us).
        new DropCollectionCommand({dbName: testDb, collName: targetColl}).execute(st.s);

        csTest.cleanUp();
    });
});
