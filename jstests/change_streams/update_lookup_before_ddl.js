/**
 * Tests that the change stream with 'fullDocument: updateLookup' option performs the lookup only by
 * nss by default and does an additional collection UUID check when
 * 'matchCollectionUUIDForUpdateLookup: true' option is set. Also covers the collection- and
 * database-gone cases (dropCollection, dropDatabase), which must report a null fullDocument.
 *
 * The failpoint-orchestrated mid-batch DDL and interrupt cases live in
 * jstests/noPassthrough/query/change_stream_update_lookup_midbatch.js: their parallel-shell
 * getMores and exact batch counts do not survive passthrough overrides.
 *
 * @tags: [
 *   # The exact change-event and metrics-delta assertions no longer hold once the txn
 *   # passthrough bundles the test's writes into transactions.
 *   change_stream_does_not_expect_txns,
 * ]
 */
import {
    assertCreateCollection,
    assertDropCollection,
    assertDropAndRecreateCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {beforeEach, afterEach, describe, it} from "jstests/libs/mochalite.js";
import {withChangeStreamTest} from "jstests/libs/query/change_stream_util.js";
import {
    expectedUpdateLookupEngine,
    readUpdateLookupDelta,
    ServerStatusMetrics,
} from "jstests/libs/query/change_stream_metrics_util.js";

describe("change stream", function () {
    const testDB = db.getSiblingDB(jsTestName());
    const collNameA = "collA";
    const collNameB = "collB";

    // The single source of truth for which DDL ops this test covers. Each entry names the op, says
    // whether it needs a sharded cluster (requiresSharding) plus three facts the driver needs to
    // compute the expected outcome:
    //   - changesUuid: does the op give collA a different UUID than before? (a collection that no
    //     longer exists also counts: there is trivially no UUID left for anything to match).
    //   - collectionExists: is there a collection at collA once 'run' returns?
    //   - shouldRecreateCollection: did 'run' make the old collA go away outright (rename-away,
    //     drop), requiring it to be recreated and insert a replacement document?
    const ddlOps = [
        {
            name: "renameCollection",
            requiresSharding: false,
            changesUuid: true,
            collectionExists: true,
            shouldRecreateCollection: true,
            canRun: () => true,
            run: () =>
                assert.commandWorked(testDB.getCollection(collNameA).renameCollection(collNameB)),
        },
        {
            name: "dropCollection",
            requiresSharding: false,
            changesUuid: true, // no collection remains, so trivially no UUID can match.
            // Left absent (unlike renameCollection, which recreates collA under a new UUID): this
            // exercises the collection-acquisition NamespaceNotFound mapping specifically, as
            // distinct from dropDatabase's routing-layer NamespaceNotFound.
            collectionExists: false,
            shouldRecreateCollection: false,
            canRun: () => true,
            run: () => assertDropCollection(testDB, collNameA),
        },
        {
            name: "dropDatabase",
            requiresSharding: false,
            changesUuid: true, // no database remains, so trivially no UUID can match.
            collectionExists: false,
            shouldRecreateCollection: false,
            canRun: () => true,
            run: () => assert.commandWorked(testDB.dropDatabase()),
        },
        {
            name: "shardCollection",
            requiresSharding: true,
            // Sharding the unsharded collection does not change its UUID.
            changesUuid: false,
            collectionExists: true,
            shouldRecreateCollection: false,
            canRun: () => !FixtureHelpers.isSharded(testDB.getCollection(collNameA)),
            run: () =>
                assert.commandWorked(
                    testDB.adminCommand({
                        shardCollection: testDB.getCollection(collNameA).getFullName(),
                        key: {_id: 1},
                        numInitialChunks: 2,
                    }),
                ),
        },
        {
            name: "reshardCollection",
            requiresSharding: true,
            // Resharding generates a new collection with the same name but a different UUID.
            changesUuid: true,
            collectionExists: true,
            shouldRecreateCollection: false,
            canRun: () => FixtureHelpers.isSharded(testDB.getCollection(collNameA)),
            run: () =>
                assert.commandWorked(
                    testDB.adminCommand({
                        reshardCollection: testDB.getCollection(collNameA).getFullName(),
                        key: {_id: 1},
                        numInitialChunks: 2,
                    }),
                ),
        },
    ];

    // Which DDL ops can run depends on the fixture, so this is computed inside each test rather
    // than at describe registration time.
    const configsFor = () =>
        ddlOps
            .filter((op) => !op.requiresSharding || FixtureHelpers.isMongos(testDB))
            .flatMap((op) => {
                const opts = [
                    {},
                    {matchCollectionUUIDForUpdateLookup: false},
                    {matchCollectionUUIDForUpdateLookup: true},
                ];
                return opts.map((changeStreamOptions) => ({op, changeStreamOptions}));
            });

    beforeEach(function () {
        assertDropAndRecreateCollection(testDB, collNameA);
    });

    afterEach(function () {
        assertDropCollection(testDB, collNameA);
        assertDropCollection(testDB, collNameB);
    });

    for (const config of configsFor()) {
        it(`running ${config.op.name} before updateLookup`, function () {
            const {op, changeStreamOptions} = config;
            jsTest.log.info("Running change stream with update lookup test after DDL", {
                op: op.name,
                changeStreamOptions,
            });

            withChangeStreamTest(testDB, (cst) => {
                let cursor = cst.startWatchingChanges({
                    pipeline: [
                        {$changeStream: {...changeStreamOptions, fullDocument: "updateLookup"}},
                    ],
                    collection: collNameA,
                });

                // Insert 'doc' into 'collA' and ensure it is seen in the change stream.
                const doc = {_id: 0, a: 1};
                assert.commandWorked(testDB.getCollection(collNameA).insert(doc));
                let expected = {
                    documentKey: {_id: doc._id},
                    fullDocument: doc,
                    ns: {db: testDB.getName(), coll: collNameA},
                    operationType: "insert",
                };
                cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

                // Update the 'doc' in order to generate the update event.
                assert.commandWorked(
                    testDB.getCollection(collNameA).update({_id: doc._id}, {$inc: {a: 1}}),
                );
                const updatedDocInCollA = {...doc, a: 2};

                if (!op.canRun()) {
                    jsTest.log.info("Test case is not compatible in this suite; early exit", {
                        op: op.name,
                    });
                    return;
                }

                // Only inserted (and only relevant to the expected fullDocument below) when
                // 'op.shouldRecreateCollection' is set.
                const newDocInNewCollA = {
                    ...doc,
                    b: "extra field in the new document in the new collection",
                };

                let expectedFullDocument;
                const delta = ServerStatusMetrics.withServerStatusMetricsAcrossCluster(
                    testDB,
                    () => {
                        op.run();
                        if (op.shouldRecreateCollection) {
                            assertCreateCollection(testDB, collNameA);
                            assert.commandWorked(
                                testDB.getCollection(collNameA).insert(newDocInNewCollA),
                            );
                        }

                        // If this test is running with secondary read preference, it's necessary for the
                        // update to propagate to all secondary nodes and be available for majority reads
                        // before we can assume looking up the document will succeed.
                        FixtureHelpers.awaitLastOpCommitted(testDB);

                        // The document currently sitting at collA's namespace, regardless of
                        // whether updateLookup can actually see it (below).
                        const currentDoc = op.shouldRecreateCollection
                            ? newDocInNewCollA
                            : updatedDocInCollA;

                        // updateLookup can't see it when there's no collection left to look up, or
                        // when the stream demands an exact UUID match and the op just changed it.
                        const uuidMismatch =
                            op.changesUuid &&
                            changeStreamOptions.matchCollectionUUIDForUpdateLookup;
                        expectedFullDocument =
                            op.collectionExists && !uuidMismatch ? currentDoc : null;
                        expected = {
                            documentKey: {_id: doc._id},
                            fullDocument: expectedFullDocument,
                            ns: {db: testDB.getName(), coll: collNameA},
                            operationType: "update",
                        };
                        cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});
                    },
                );

                // The optimized executors (Express, SBE) resolve found/notFound directly and
                // never decline on a UUID mismatch, so the predicted engine must have handled the
                // lookup with no fallback. The document may be notHandled by the main executor due
                // to stale CRI, resulting in the use of the aggregation fallback engine.
                const mainLookupEngine = expectedUpdateLookupEngine();
                const metricsByEngine = readUpdateLookupDelta(delta);
                const predictedEngineHandledIt =
                    metricsByEngine[mainLookupEngine].found +
                        metricsByEngine[mainLookupEngine].notFound ===
                    1;
                const engineThatHandledLookup =
                    !predictedEngineHandledIt &&
                    metricsByEngine.aggregation.found + metricsByEngine.aggregation.notFound === 1
                        ? "aggregation"
                        : mainLookupEngine;

                // Assert that found/notFound metrics are captured correctly for the 'expectedFullDocument'.
                const expectFound = expectedFullDocument !== null;
                assert.eq(metricsByEngine[engineThatHandledLookup].found, expectFound ? 1 : 0, {
                    engineThatHandledLookup,
                    metricsByEngine,
                });
                assert.eq(metricsByEngine[engineThatHandledLookup].notFound, expectFound ? 0 : 1, {
                    engineThatHandledLookup,
                    metricsByEngine,
                });

                for (const [otherEngine, otherLookupStats] of Object.entries(metricsByEngine)) {
                    if (otherEngine === engineThatHandledLookup) {
                        continue;
                    }

                    // The predicted engine is only allowed a single declined lookup, and only when
                    // the aggregation fallback ended up handling it instead.
                    const allowedNotHandled =
                        engineThatHandledLookup === "aggregation" &&
                        otherEngine === mainLookupEngine
                            ? 1
                            : 0;
                    assert.eq(otherLookupStats.found, 0, {
                        otherEngine,
                        otherLookupStats,
                        delta,
                    });
                    assert.eq(otherLookupStats.notFound, 0, {
                        otherEngine,
                        otherLookupStats,
                        delta,
                    });
                    assert.eq(otherLookupStats.notHandled, allowedNotHandled, {
                        otherEngine,
                        otherLookupStats,
                        delta,
                    });
                }
            });
        });
    }
});
