/**
 * Tests that a change stream can resume correctly after rename invalidation.
 *
 * @tags: [
 *   requires_sharding,
 *   uses_change_streams,
 *   assumes_balancer_off,
 *   does_not_support_stepdowns,
 * ]
 */
import {describe, it, before, afterEach, after} from "jstests/libs/mochalite.js";
import {ChangeStreamWatchMode} from "jstests/libs/query/change_stream_util.js";
import {ChangeStreamReader, ChangeStreamReadingMode} from "jstests/libs/util/change_stream/change_stream_reader.js";
import {Writer} from "jstests/libs/util/change_stream/change_stream_writer.js";
import {Connector} from "jstests/libs/util/change_stream/change_stream_connector.js";
import {
    CreateDatabaseCommand,
    CreateUnsplittableCollectionCommand,
    InsertDocCommand,
    RenameToNonExistentSameDbCommand,
} from "jstests/libs/util/change_stream/change_stream_commands.js";
import {
    buildExpectedEvents,
    createMatcher,
    createShardingTest,
    getCurrentClusterTime,
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
        const commands = [
            new CreateDatabaseCommand(testDb, testColl, [shards[0]], null),
            new CreateUnsplittableCollectionCommand(testDb, testColl, [shards[1]], {exists: false, isSharded: false}),
            new RenameToNonExistentSameDbCommand(testDb, testColl, shards, {exists: true, isSharded: false}),
            new InsertDocCommand(testDb, testColl, shards, {exists: false, isSharded: false}),
        ];
        const expectedEvents = buildExpectedEvents(commands, ChangeStreamWatchMode.kCollection);
        const startTime = getCurrentClusterTime(st.s);

        // Execute commands via Writer.
        const writerName = "writer_rename_resume";
        Writer.run(st.s, writerName, commands);

        // Read events via ChangeStreamReader (handles invalidate + startAfter resume).
        const readerName = "reader_rename_resume";
        const readerConfig = {
            dbName: testDb,
            collName: testColl,
            watchMode: ChangeStreamWatchMode.kCollection,
            readingMode: ChangeStreamReadingMode.kContinuous,
            startAtClusterTime: startTime,
            numberOfEventsToRead: expectedEvents.length,
            instanceName: readerName,
        };
        ChangeStreamReader.run(st.s, readerConfig);

        // Build VerifierContext and run SingleReaderVerificationTestCase.
        const ctx = new VerifierContext({[readerName]: readerConfig}, {[readerName]: createMatcher(expectedEvents)});
        new SingleReaderVerificationTestCase(readerName).run(st.s, ctx);
    });
});
