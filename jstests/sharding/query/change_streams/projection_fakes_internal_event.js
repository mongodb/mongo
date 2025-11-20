/**
 * Tests that a user projection which fakes an internal topology-change event is handled gracefully
 * in a sharded cluster.
 * TODO SERVER-65778: rework this test when we can handle faked internal events more robustly.
 *
 * Tests that if a user fakes an internal event with a projection nothing crashes, so not valuable
 * to test with a config shard.
 * @tags: [assumes_read_preference_unchanged, config_shard_incompatible]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {describe, it, before, after} from "jstests/libs/mochalite.js";
import {assertCreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {ChangeStreamTest} from "jstests/libs/query/change_stream_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

describe("$changeStream", function () {
    const numShards = 2;
    const configDotShardsNs = {
        db: "config",
        coll: "shards",
    };

    let st;
    let adminDB;
    let testColl;

    const startAtOperationTime = Timestamp(1, 1);

    let existingShardWrongNameDoc;
    let existingShardWrongHostDoc;
    let existingShardDoc;
    let invalidShardDoc;
    let fakeShardDoc;

    before(function () {
        st = new ShardingTest({
            shards: numShards,
            rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
        });

        const mongosConn = st.s;

        const testDB = mongosConn.getDB(jsTestName());
        adminDB = mongosConn.getDB("admin");
        testColl = assertCreateCollection(testDB, "test");

        // Insert one test document that points to a valid shard, and one that points to an invalid shard.
        // These will generate change events that look identical to a config.shards entry, except for 'ns'.
        // It also means that the documentKey field in the resume token will look like a potentially valid
        // new-shard document.
        existingShardDoc = testDB.getSiblingDB("config").shards.find({_id: st.rs0.name}).next();
        existingShardWrongNameDoc = {
            _id: "nonExistentName",
            host: existingShardDoc.host,
        };
        existingShardWrongHostDoc = {
            ...existingShardDoc,
            _id: st.rs1.name,
            host: `${st.rs1.name}/${st.rs1.host}-wrong:${st.rs1.ports[0]}`,
        };
        fakeShardDoc = {
            _id: "shardX",
            host: "shardX/nonExistentHost:27017",
        };
        invalidShardDoc = {
            _id: "shardY",
            host: null,
        };

        assert.commandWorked(testColl.insert(existingShardWrongNameDoc));
        assert.commandWorked(testColl.insert(existingShardWrongHostDoc));
        assert.commandWorked(testColl.insert(existingShardDoc));
        assert.commandWorked(testColl.insert(invalidShardDoc));
        assert.commandWorked(testColl.insert(fakeShardDoc));

        // TODO: SERVER-113286 Generate NamespacePlacementChanged on addition of first shard in the sharded cluster.
        if (FeatureFlagUtil.isPresentAndEnabled(testDB, "ChangeStreamPreciseShardTargeting")) {
            assert.commandWorked(testDB.adminCommand({resetPlacementHistory: 1}));
        }

        // Log the shard description documents that we just inserted into the collection.
        jsTest.log.info("Shard docs: ", {docs: testColl.find().toArray()});
    });

    after(function () {
        st.stop();
    });

    describe("returns nothing when projecting valid internal events for changeStreams v1, but returns all of them in v2", function () {
        // Helper function which opens a stream with the given projection and asserts that its behaviour
        // conforms to the specified arguments; it will return the expected events. Passing an empty array will confirm that we see no events in the stream. We
        // further confirm that the faked events do not cause additional cursors to be opened.
        function assertChangeStreamBehaviour(projection, expectedEvents) {
            // Generate a random ID for this stream.
            const commentID = `${Math.random()}`;

            // Create a change stream cursor with the specified projection.
            let csCursor = testColl.watch([{$addFields: projection}], {
                startAtOperationTime,
                comment: commentID,
            });

            const changeStreamVersion = csCursor.getChangeStreamVersion();
            const expectedEventsForGivenChangeStreamVersion = expectedEvents[changeStreamVersion];

            // Confirm that the observed events match the expected events, if specified.
            if (expectedEventsForGivenChangeStreamVersion.length > 0) {
                for (let expectedEvent of expectedEventsForGivenChangeStreamVersion) {
                    assert.soon(() => csCursor.hasNext());
                    const nextEvent = csCursor.next();
                    for (let fieldName in expectedEvent) {
                        assert.eq(expectedEvent[fieldName], nextEvent[fieldName], {expectedEvent, nextEvent});
                    }
                }
            } else {
                // If there are no expected events, confirm that the token advances without seeing anything.
                const startPoint = csCursor.getResumeToken();
                assert.soon(() => {
                    assert(!csCursor.hasNext(), () => tojson(csCursor.next()));
                    return bsonWoCompare(csCursor.getResumeToken(), startPoint) > 0;
                });
            }

            // Otherwise, confirm that we still only have a single cursor on each shard. It's possible
            // that the same cursor will be listed as both active and inactive, so group by cursorId.
            const openCursors = adminDB
                .aggregate([
                    {$currentOp: {idleCursors: true}},
                    {$match: {"cursor.originatingCommand.comment": commentID}},
                    {
                        $group: {
                            _id: {shard: "$shard", cursorId: "$cursor.cursorId"},
                            currentOps: {$push: "$$ROOT"},
                        },
                    },
                ])
                .toArray();

            const assertFn = changeStreamVersion === "v1" ? assert.eq : assert.lte;
            assertFn(
                openCursors.length,
                numShards,
                // Dump all the running operations for better debuggability.
                () => tojson(adminDB.aggregate([{$currentOp: {idleCursors: true}}]).toArray()),
            );

            // Close the change stream when we are done.
            csCursor.close();
        }

        it("Test that a projection which fakes a 'migrateChunkToNewShard' event is swallowed but has no effect", function () {
            assertChangeStreamBehaviour(
                {operationType: "migrateChunkToNewShard"},
                {
                    v1: [],
                    v2: [
                        {operationType: "migrateChunkToNewShard"},
                        {operationType: "migrateChunkToNewShard"},
                        {operationType: "migrateChunkToNewShard"},
                        {operationType: "migrateChunkToNewShard"},
                        {operationType: "migrateChunkToNewShard"},
                    ],
                },
            );
        });

        it("Test that a projection which fakes an event on config.shards with a non-string operationType is allowed to pass through", function () {
            const testProjection = {
                ns: configDotShardsNs,
                operationType: null,
            };
            const expectedEvents = [
                {operationType: null, fullDocument: existingShardWrongNameDoc},
                {operationType: null, fullDocument: existingShardWrongHostDoc},
                {operationType: null, fullDocument: existingShardDoc},
                {operationType: null, fullDocument: invalidShardDoc},
                {operationType: null, fullDocument: fakeShardDoc},
            ];
            assertChangeStreamBehaviour(testProjection, {v1: expectedEvents, v2: expectedEvents});
        });

        it("Test that a projection which fakes an event on config.shards with a non-timestamp clusterTime is allowed to pass through", function () {
            const testProjection = {
                ns: configDotShardsNs,
                clusterTime: null,
            };
            const expectedEvents = [
                {clusterTime: null, fullDocument: existingShardWrongNameDoc},
                {clusterTime: null, fullDocument: existingShardWrongHostDoc},
                {clusterTime: null, fullDocument: existingShardDoc},
                {clusterTime: null, fullDocument: invalidShardDoc},
                {clusterTime: null, fullDocument: fakeShardDoc},
            ];
            assertChangeStreamBehaviour(testProjection, {v1: expectedEvents, v2: expectedEvents});
        });

        it("Test that a projection which fakes an event on config.shards with a non-object fullDocument is allowed to pass through", function () {
            const testProjection = {
                ns: configDotShardsNs,
                fullDocument: null,
            };
            const expectedEvents = [
                {fullDocument: null},
                {fullDocument: null},
                {fullDocument: null},
                {fullDocument: null},
                {fullDocument: null},
            ];
            assertChangeStreamBehaviour(testProjection, {v1: expectedEvents, v2: expectedEvents});
        });

        it("Test that a projection which fakes a new-shard event on config.shards with a valid fullDocument pointing to an existing shard is swallowed but has no effect", function () {
            const testProjection = {
                ns: configDotShardsNs,
                fullDocument: existingShardDoc,
            };
            assertChangeStreamBehaviour(testProjection, {
                v1: [],
                v2: [
                    {fullDocument: existingShardDoc},
                    {fullDocument: existingShardDoc},
                    {fullDocument: existingShardDoc},
                    {fullDocument: existingShardDoc},
                    {fullDocument: existingShardDoc},
                ],
            });
        });

        it("Test that a projection which fakes a new-shard event on config.shards with a valid fullDocument pointing to an existing shard's name, but the wrong host, is swallowed and has no effect", function () {
            const testProjection = {
                ns: configDotShardsNs,
                fullDocument: existingShardWrongHostDoc,
            };
            assertChangeStreamBehaviour(testProjection, {
                v1: [],
                v2: [
                    {fullDocument: existingShardWrongHostDoc},
                    {fullDocument: existingShardWrongHostDoc},
                    {fullDocument: existingShardWrongHostDoc},
                    {fullDocument: existingShardWrongHostDoc},
                    {fullDocument: existingShardWrongHostDoc},
                ],
            });
        });
    });

    describe("Throws an exception when handling shard insertion document coming from the shard in invalid format", function () {
        function assertChangeStreamShouldThrowForV1(projection, expectedErrorCode) {
            const isMultiversion =
                TestData.useRandomBinVersionsWithinReplicaSet || TestData.mixedBinVersions || TestData.mongosBinVersion;
            if (isMultiversion) {
                return;
            }

            // Generate a random ID for this stream.
            const commentID = `${Math.random()}`;
            const testDB = st.s.getDB(jsTestName());

            // Create a change stream cursor with the specified projection.
            let csCursor = testColl.watch([{$addFields: projection}], {
                startAtOperationTime,
                version: "v1",
                comment: commentID,
            });

            assert.throwsWithCode(() => {
                let res = csCursor;
                assert.soon(() => {
                    const cursorId = res._cursorid ?? res.cursor.id;
                    res = assert.commandWorked(testDB.runCommand({getMore: cursorId, collection: "test"}));
                });
            }, [expectedErrorCode, undefined]);
        }

        it("Test that a projection which fakes a new-shard event on config.shards with a valid fullDocument pointing to an existing shard's host, but the wrong shard name, throws as it attempts to connect", function () {
            const testProjection = {
                ns: configDotShardsNs,
                fullDocument: existingShardWrongNameDoc,
            };
            assertChangeStreamShouldThrowForV1(testProjection, ErrorCodes.ShardNotFound);
        });

        it("Test that a projection which fakes a new-shard event on config.shards with a valid fullDocument pointing to a non-existent shard throws as it attempts to connect", function () {
            const testProjection = {
                ns: configDotShardsNs,
                fullDocument: fakeShardDoc,
            };
            assertChangeStreamShouldThrowForV1(testProjection, ErrorCodes.ShardNotFound);
        });

        it("Test that a projection which fakes a new-shard event on config.shards with an invalid fullDocument throws a validation exception", function () {
            const testProjection = {
                ns: configDotShardsNs,
                fullDocument: invalidShardDoc,
            };
            assertChangeStreamShouldThrowForV1(testProjection, ErrorCodes.TypeMismatch);
        });
    });

    describe("returns projected 'createDatabase' events", function () {
        let csTest;
        let tmpColl;

        const databaseCreatedEventForExistingDbProjection = {
            operationType: "insert",
            ns: {db: "config", coll: "databases"},
            fullDocument: {_id: "tmp"},
        };

        const databaseCreatedEventForNonExistingDbProjection = {
            operationType: "insert",
            ns: {db: "config", coll: "databases"},
            fullDocument: {_id: "non-existent-db"},
        };

        const invalidDatabaseCreatedEventProjection = {
            operationType: "insert",
            ns: {coll: "databases"},
        };

        before(function () {
            const testDB = st.s.getDB(jsTestName());
            csTest = new ChangeStreamTest(testDB);
            tmpColl = assertCreateCollection(testDB, "tmp");

            assert.commandWorked(tmpColl.insert({a: 1}));
            assert.commandWorked(tmpColl.insert({a: 2}));
        });

        it("for existing database", function () {
            // Open change stream over 'tmpColl'.
            const cursor = csTest.startWatchingChanges({
                pipeline: [{$project: databaseCreatedEventForExistingDbProjection}],
                collection: tmpColl,
                startAtOperationTime,
            });

            // Ensure that the event is not swallowed and instead the insert is observed.
            csTest.assertNextChangesEqual({
                cursor,
                expectedChanges: databaseCreatedEventForExistingDbProjection,
            });

            csTest.cleanUp();
        });

        it("for non-existing database", function () {
            // Open change stream over 'tmpColl'.
            const cursor = csTest.startWatchingChanges({
                pipeline: [{$project: databaseCreatedEventForNonExistingDbProjection}],
                collection: tmpColl,
                startAtOperationTime,
            });

            // Ensure that the event is not swallowed and instead the insert is observed.
            csTest.assertNextChangesEqual({
                cursor,
                expectedChanges: databaseCreatedEventForNonExistingDbProjection,
            });

            csTest.cleanUp();
        });

        it("for invalid createDatabase event", function () {
            // Open change stream over 'tmpColl'.
            const cursor = csTest.startWatchingChanges({
                pipeline: [{$project: invalidDatabaseCreatedEventProjection}],
                collection: tmpColl,
                startAtOperationTime,
            });

            // Ensure that the event is not swallowed and instead the insert is observed.
            csTest.assertNextChangesEqual({
                cursor,
                expectedChanges: invalidDatabaseCreatedEventProjection,
            });

            csTest.cleanUp();
        });
    });
});
