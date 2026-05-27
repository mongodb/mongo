/**
 * Verifies that a cross-DB rename immediately following moveCollection on an untracked collection
 * produces exactly one 'drop' change stream event. Before the fix, a second phantom 'drop' was
 * emitted because the donor shard's source drop in renameCollectionAcrossDatabases did not honour
 * the markFromMigrate flag set by the rename coordinator.
 *
 * Uses the Writer/ChangeStreamReader/Verifier framework so that:
 * - expected events are derived from getChangeEvents() on each Command object,
 * - the SingleChangeStreamMatcher enforces the exact ordered sequence, and
 * - an extra 'drop' (the bug) would cause an immediate mismatch failure.
 *
 * @tags: [
 *   assumes_balancer_off,
 *   does_not_support_stepdowns,
 *   featureFlagChangeStreamPreciseShardTargeting,
 *   multiversion_incompatible,
 *   requires_sharding,
 *   uses_change_streams,
 * ]
 */
import {after, afterEach, before, describe, it} from "jstests/libs/mochalite.js";
import {ChangeStreamWatchMode, getClusterTime} from "jstests/libs/query/change_stream_util.js";
import {
    CreateDatabaseCommand,
    CreateUntrackedCollectionCommand,
    InsertDocCommand,
    MoveCollectionCommand,
    RenameToNonExistentDifferentDbCommand,
} from "jstests/libs/util/change_stream/change_stream_commands.js";
import {ChangeStreamReader, ChangeStreamReadingMode} from "jstests/libs/util/change_stream/change_stream_reader.js";
import {
    buildExpectedEvents,
    createMatcher,
    createShardingTest,
    runTeardownSteps,
} from "jstests/libs/util/change_stream/change_stream_sharding_utils.js";
import {Connector} from "jstests/libs/util/change_stream/change_stream_connector.js";
import {Verifier, SingleReaderVerificationTestCase} from "jstests/libs/util/change_stream/change_stream_verifier.js";
import {Writer} from "jstests/libs/util/change_stream/change_stream_writer.js";

describe("change stream cross-db rename after moveCollection", function () {
    let st, shards, mongos;

    before(function () {
        st = createShardingTest(/*mongos*/ 1, /*shards*/ 2);
        shards = assert.commandWorked(st.s.adminCommand({listShards: 1})).shards;
        mongos = st.s;
    });

    afterEach(function () {
        runTeardownSteps(
            () => Writer.joinAll(),
            () => ChangeStreamReader.joinAll(),
            () => Connector.cleanupAll(mongos),
            () => mongos.getDB(jsTestName()).dropDatabase(),
        );
    });

    after(function () {
        st.stop();
    });

    it("emits exactly one drop event", function () {
        const dbName = jsTestName();
        const collName = "coll";
        const watchMode = ChangeStreamWatchMode.kDb;

        new CreateDatabaseCommand({dbName, shardSet: shards, primaryShard: shards[0]._id}).execute(mongos);

        // Fixed command sequence: create → insert → moveCollection → cross-DB rename.
        // getChangeEvents(kDb) for each yields: create, insert, reshardCollection, drop.
        // An extra 'drop' (the pre-fix bug) would cause a mismatch in the Verifier.
        const commands = [
            new CreateUntrackedCollectionCommand({dbName, collName}),
            new InsertDocCommand({dbName, collName, collectionCtx: {exists: true}}),
            new MoveCollectionCommand({
                dbName,
                collName,
                shardSet: shards,
                collectionCtx: {exists: true, isUnsplittable: false},
            }),
            new RenameToNonExistentDifferentDbCommand({
                dbName,
                collName,
                shardSet: shards,
                collectionCtx: {exists: true},
                dropAfterRename: true,
            }),
        ];

        const expectedEvents = buildExpectedEvents(commands, watchMode);

        const startTime = getClusterTime(mongos.getDB("admin"));

        const readerName = `reader_cross_db_rename_${dbName}`;
        const readerConfig = {
            instanceName: readerName,
            watchMode,
            dbName,
            collName,
            numberOfEventsToRead: expectedEvents.length,
            readingMode: ChangeStreamReadingMode.kContinuous,
            startAtClusterTime: startTime,
            excludeOperationTypes: ["createIndexes", "dropIndexes", "modify"],
        };

        Writer.run(mongos, `writer_cross_db_rename_${dbName}`, commands, 0);
        ChangeStreamReader.run(mongos, readerConfig);

        new Verifier().run(
            mongos,
            {
                changeStreamReaderConfigs: {[readerName]: readerConfig},
                matcherSpecsByInstance: {
                    [readerName]: createMatcher(expectedEvents),
                },
                instanceName: `verifier_cross_db_rename_${dbName}`,
            },
            [new SingleReaderVerificationTestCase(readerName)],
        );
    });
});
