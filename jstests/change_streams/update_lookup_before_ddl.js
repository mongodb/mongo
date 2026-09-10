/**
 * Tests that the change stream with 'fullDocument: updateLookup' option performs the lookup only by
 * nss by default and does an additional collection UUID check when
 * 'matchCollectionUUIDForUpdateLookup: true' option is set.
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
    // Which DDL ops can run depends on the fixture, so this is computed inside each test rather
    // than at describe registration time.
    const ddlOpsFor = () =>
        FixtureHelpers.isMongos(testDB)
            ? ["renameCollection", "shardCollection", "reshardCollection"]
            : ["renameCollection"];
    const configsFor = () =>
        ddlOpsFor().flatMap((op) => {
            const opts = [
                {},
                {matchCollectionUUIDForUpdateLookup: false},
                {matchCollectionUUIDForUpdateLookup: true},
            ];
            return opts.map((changeStreamOptions) => ({op, changeStreamOptions}));
        });

    const collNameA = "collA";
    const collNameB = "collB";

    beforeEach(function () {
        assertDropAndRecreateCollection(testDB, collNameA);
    });

    afterEach(function () {
        assertDropCollection(testDB, collNameA);
        assertDropCollection(testDB, collNameB);
    });

    for (const config of configsFor()) {
        it(`running ${config.op} before updateLookup`, function () {
            const {op, changeStreamOptions} = config;
            jsTest.log.info("Running change stream with update lookup test after DDL", {
                op,
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

                // When 'matchCollectionUUIDForUpdateLookup' is true, no 'fullDocument' should be
                // returned if the collection on which 'updateLookup' is performed has a different
                // UUID from the collection on which the change stream was opened. When it is false
                // or unset, return the latest document from the collection with the same namespace.
                let expectedFullDocument;
                let earlyExit = false;
                const delta = ServerStatusMetrics.withServerStatusMetricsAcrossCluster(
                    testDB,
                    () => {
                        if (op === "renameCollection") {
                            // Rename collection collA -> collB.
                            assert.commandWorked(
                                testDB.getCollection(collNameA).renameCollection(collNameB),
                            );

                            // Create new collection with the old name, "collA", (yet UUID will be
                            // different) and insert document with the same id.
                            assertCreateCollection(testDB, collNameA);
                            const newDocInNewCollA = {
                                ...doc,
                                b: "extra field in the new document in the new collection",
                            };
                            assert.commandWorked(
                                testDB.getCollection(collNameA).insert(newDocInNewCollA),
                            );
                            expectedFullDocument =
                                changeStreamOptions.matchCollectionUUIDForUpdateLookup
                                    ? null
                                    : newDocInNewCollA;
                        } else if (op === "shardCollection") {
                            if (FixtureHelpers.isSharded(testDB.getCollection(collNameA))) {
                                jsTest.log.info(
                                    "Early exit, because collection 'collA' is already sharded",
                                );
                                earlyExit = true;
                                return;
                            }

                            // Sharding the unsharded collection does not change its UUID, therefore
                            // regardless of 'matchCollectionUUIDForUpdateLookup' being set or not, we
                            // should observe the 'updatedDocInCollA' on updateLookup.
                            assert.commandWorked(
                                testDB.adminCommand({
                                    shardCollection: testDB.getCollection(collNameA).getFullName(),
                                    key: {_id: 1},
                                    numInitialChunks: 2,
                                }),
                            );
                            expectedFullDocument = updatedDocInCollA;
                        } else if (op === "reshardCollection") {
                            if (!FixtureHelpers.isSharded(testDB.getCollection(collNameA))) {
                                jsTest.log.info(
                                    "Early exit, because collection 'collA' is not sharded",
                                );
                                earlyExit = true;
                                return;
                            }

                            // Reshard the collection in order to generate the new collection with the
                            // same name, but different UUID.
                            assert.commandWorked(
                                testDB.adminCommand({
                                    reshardCollection: testDB
                                        .getCollection(collNameA)
                                        .getFullName(),
                                    key: {_id: 1},
                                    numInitialChunks: 2,
                                }),
                            );
                            expectedFullDocument =
                                changeStreamOptions.matchCollectionUUIDForUpdateLookup
                                    ? null
                                    : updatedDocInCollA;
                        }

                        // If this test is running with secondary read preference, it's necessary for the
                        // update to propagate to all secondary nodes and be available for majority reads
                        // before we can assume looking up the document will succeed.
                        FixtureHelpers.awaitLastOpCommitted(testDB);

                        expected = {
                            documentKey: {_id: doc._id},
                            fullDocument: expectedFullDocument,
                            ns: {db: testDB.getName(), coll: collNameA},
                            operationType: "update",
                        };
                        cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});
                    },
                );

                if (earlyExit) {
                    jsTest.log.info("Test case is not compatible in this suite; early exit", {op});
                    return;
                }

                // The optimized executors (Express, SBE) resolve found/notFound directly and
                // never decline on a UUID mismatch, so the selected engine must have handled the
                // lookup with no fallback.
                const engine = expectedUpdateLookupEngine();
                const metricsByEngine = readUpdateLookupDelta(delta);
                const expectFound = expectedFullDocument !== null;
                assert.eq(metricsByEngine[engine].found, expectFound ? 1 : 0, {
                    engine,
                    metricsByEngine,
                });
                assert.eq(metricsByEngine[engine].notFound, expectFound ? 0 : 1, {
                    engine,
                    metricsByEngine,
                });
                assert.eq(metricsByEngine[engine].notHandled, 0, {engine, metricsByEngine});

                for (const [other, otherLookup] of Object.entries(metricsByEngine)) {
                    if (other === engine) {
                        continue;
                    }
                    assert.eq(
                        otherLookup.found + otherLookup.notFound + otherLookup.notHandled,
                        0,
                        {
                            other,
                            otherLookup,
                            delta,
                        },
                    );
                }
            });
        });
    }
});
