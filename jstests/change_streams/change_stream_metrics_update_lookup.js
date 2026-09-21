/**
 * Verifies that enriching update events with a post-image (fullDocument: "updateLookup") records the
 * single-document-lookup outcome into the per-engine serverStatus metrics under
 * 'changeStreams.updateLookup.<engine>'.
 *
 * @tags: [
 *   # Can not run in balancer suites as it expects no data migrations across shards for correct
 *   # metric capture (test expects shard local lookup).
 *   assumes_balancer_off,
 *   # The exact change-event and metrics-delta assertions no longer hold once the txn
 *   # passthrough bundles the test's writes into transactions.
 *   change_stream_does_not_expect_txns,
 *   requires_fcv_90,
 *   assumes_no_implicit_cursor_exhaustion,
 *   # The 'gone' document's post-image lookup relies on observing the delete that immediately
 *   # follows its update. The lookup's read concern is {level: "majority", afterClusterTime:
 *   # <the update's own clusterTime T1>}, a minimum bound, not "now". A secondary servicing
 *   # that read may not yet have majority-committed (or advanced its per-batch lastApplied past)
 *   # the later delete at T2 > T1, so it can legitimately still see the pre-delete state.
 *   assumes_read_preference_unchanged,
 * ]
 */
import {before, beforeEach, after, afterEach, describe, it} from "jstests/libs/mochalite.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {
    assertChangeStreamEventsEqWithDeploymentAwareness,
    withChangeStreamTest,
} from "jstests/libs/query/change_stream_util.js";
import {
    expectedUpdateLookupEngine,
    ServerStatusMetrics,
    UpdateLookupExecutor,
} from "jstests/libs/query/change_stream_metrics_util.js";
import {withClusteredColl, withCollation} from "jstests/libs/query/collection_config_decorators.js";

// A compound _id with a Timestamp component (to exercise non-scalar key encoding), fully derived
// from 'seed' so equal seeds yield equal ids and no field is collation-sensitive.
function compoundId(seed) {
    return {a: seed, b: Timestamp(seed, 1)};
}

describe("change stream updateLookup single-document-lookup metrics", function () {
    const configs = [
        {
            name: "scalar _id",
            collOpts: {},
            presentId: "present",
            goneId: "gone",
            isCompound: false,
        },
        {
            name: "compound _id",
            collOpts: {},
            presentId: compoundId(100),
            goneId: compoundId(200),
            isCompound: true,
        },
    ]
        .flatMap((config) => [config, withClusteredColl(config)])
        .flatMap((config) => [config, withCollation(config)]);

    for (const config of configs) {
        describe(config.name, function () {
            const testDB = db.getSiblingDB(config.name.replace(/\s+/g, "_"));
            const testColl = testDB.getCollection("test");
            const ns = {db: testDB.getName(), coll: testColl.getName()};

            before(function () {
                assertDropAndRecreateCollection(testDB, testColl.getName(), config.collOpts);
            });

            after(function () {
                assert.commandWorked(testDB.dropDatabase());
            });

            beforeEach(function () {
                assert.commandWorked(
                    testColl.insert([{_id: config.presentId}, {_id: config.goneId}]),
                );
            });

            afterEach(function () {
                assert.commandWorked(testColl.deleteMany({}));
            });

            it("records found / notFound into the engine's single-document-lookup cell", function () {
                const engine = expectedUpdateLookupEngine();

                const delta = ServerStatusMetrics.withServerStatusMetricsAcrossCluster(
                    testDB,
                    () => {
                        withChangeStreamTest(testDB, (cst) => {
                            const cursor = cst.startWatchingChanges({
                                pipeline: [{$changeStream: {fullDocument: "updateLookup"}}],
                                collection: testColl.getName(),
                            });

                            // presentId still exists when the post-image is looked up ->
                            // recordFound.
                            assert.commandWorked(
                                testColl.update({_id: config.presentId}, {$set: {v: 1}}),
                            );

                            // goneId is deleted before we drain the stream, so its update event's
                            // post-image lookup finds nothing -> recordNotFound.
                            assert.commandWorked(
                                testColl.update({_id: config.goneId}, {$set: {v: 1}}),
                            );
                            assert.commandWorked(testColl.remove({_id: config.goneId}));

                            cst.assertNextChangesEqualWithDeploymentAwareness({
                                cursor,
                                expectedChanges: [
                                    {
                                        operationType: "update",
                                        ns,
                                        documentKey: {_id: config.presentId},
                                        fullDocument: {_id: config.presentId, v: 1},
                                    },
                                    {
                                        operationType: "update",
                                        ns,
                                        documentKey: {_id: config.goneId},
                                        fullDocument: null,
                                    },
                                    {
                                        operationType: "delete",
                                        ns,
                                        documentKey: {_id: config.goneId},
                                    },
                                ],
                            });
                        });
                    },
                );

                const lookup = delta.changeStreams.updateLookup[engine];
                const fallbackLookup =
                    delta.changeStreams.updateLookup[UpdateLookupExecutor.kAggregation];

                // TODO: SERVER-134080 Support non-scalar _id lookups in SbeSingleDocumentLookupExecutor.
                // This limitation is SBE-specific: Express (the primary for db/cluster-level streams)
                // handles compound _id directly, so the decline-and-fallback path below only applies
                // when SBE (i.e. a collection-level stream) is the primary.
                const isRunningSBELookup = engine === UpdateLookupExecutor.kSBE;
                if (
                    config.isCompound &&
                    !config.collOpts.hasOwnProperty("clusteredIndex") &&
                    isRunningSBELookup
                ) {
                    assert.eq(lookup.notHandled, 2, {lookup});

                    assert.eq(fallbackLookup.found, 1, {fallbackLookup});
                    assert.eq(fallbackLookup.notFound, 1, {fallbackLookup});

                    assert.gt(fallbackLookup.latencyMicros.totalCount, 0, {fallbackLookup});
                } else {
                    assert.eq(lookup.found, 1, {lookup});
                    assert.eq(lookup.notFound, 1, {lookup});

                    // Since there are no migrations, the primary executor should always succeed.
                    assert.eq(lookup.notHandled, 0, {lookup});

                    // Both lookups (found + notFound) recorded a latency observation.
                    // 'latencyMicros' is a histogram; 'totalCount' is its number of recorded
                    // observations.
                    assert.gt(lookup.latencyMicros.totalCount, 0, {lookup});
                }
            });
        });
    }

    // One shared enrichment window mixing accepted (scalar) and declined (non-clustered compound)
    // _ids, with the middle doc gone so both shape and outcome vary, in both triplet orders.
    const mixedIdConfigs = [
        {
            name: "mixed _id, scalar-compound-scalar order",
            collOpts: {},
            order: ["scalar", "compound", "scalar"],
        },
        {
            name: "mixed _id, compound-scalar-compound order",
            collOpts: {},
            order: ["compound", "scalar", "compound"],
        },
    ]
        .flatMap((config) => [config, withClusteredColl(config)])
        .flatMap((config) => [config, withCollation(config)]);

    describe("mixed scalar/compound _id batches", function () {
        const testDB = db.getSiblingDB(jsTestName());
        const testColl = testDB.getCollection("test");
        const ns = {db: testDB.getName(), coll: testColl.getName()};

        afterEach(function () {
            assert.commandWorked(testDB.dropDatabase());
        });

        for (const config of mixedIdConfigs) {
            it(`records per-document outcome for a mixed scalar/compound _id batch [${config.name}]`, function () {
                assertDropAndRecreateCollection(testDB, testColl.getName(), config.collOpts);

                // The middle document (index 1) is the one that goes missing before the lookup
                // runs; the outer two stay present.
                const docs = config.order.map((shape, i) => ({
                    shape,
                    outcome: i === 1 ? "gone" : "present",
                    id: shape === "scalar" ? `scalar${i}` : compoundId(500 + i),
                }));
                assert.commandWorked(testColl.insert(docs.map((d) => ({_id: d.id}))));

                const engine = expectedUpdateLookupEngine();
                const isClustered = config.collOpts.hasOwnProperty("clusteredIndex");
                const compoundDeclines = !isClustered && engine === UpdateLookupExecutor.kSBE;

                let actualChanges;
                let expectedChanges;
                const delta = ServerStatusMetrics.withServerStatusMetricsAcrossCluster(
                    testDB,
                    () => {
                        // See the configs comment above for why this is coll.watch() + .next()
                        // rather than ChangeStreamTest's helpers. batchSize: 0 parks everything
                        // onto the first getMore, whose unset batchSize leaves room for the
                        // remaining events to fill one window.
                        const cursor = testColl.watch([], {
                            fullDocument: "updateLookup",
                            cursor: {batchSize: 0},
                        });

                        assert.commandWorked(testColl.insert({_id: "batchWarmer"}));
                        expectedChanges = [
                            {
                                operationType: "insert",
                                ns,
                                documentKey: {_id: "batchWarmer"},
                                fullDocument: {_id: "batchWarmer"},
                            },
                        ];

                        for (const d of docs) {
                            const isGone = d.outcome === "gone";
                            assert.commandWorked(testColl.update({_id: d.id}, {$set: {v: 1}}));
                            expectedChanges.push({
                                operationType: "update",
                                ns,
                                documentKey: {_id: d.id},
                                fullDocument: isGone ? null : {_id: d.id, v: 1},
                            });

                            if (isGone) {
                                assert.commandWorked(testColl.remove({_id: d.id}));
                                expectedChanges.push({
                                    operationType: "delete",
                                    ns,
                                    documentKey: {_id: d.id},
                                });
                            }
                        }

                        // A change stream cursor never signals EOF; drain exactly the expected count.
                        actualChanges = [];
                        for (let i = 0; i < expectedChanges.length; i++) {
                            actualChanges.push(cursor.next());
                        }
                        cursor.close();
                    },
                );

                // Correctness: every event matches (ordered on replica sets, unordered on sharded
                // topologies, where cross-shard event order may not match client issue order).
                assertChangeStreamEventsEqWithDeploymentAwareness(
                    db,
                    actualChanges,
                    expectedChanges,
                );

                // fillBatch() admits exactly one event into the first window while 'shouldWaitForInserts'
                // is set (its stop-after-one break), so 2 windows over 5 events means window 1 was the
                // warmer alone and window 2 held all three updates plus the delete. Batching is a
                // property of the SBE primary, not of the collection.
                const batchingApplies = engine === UpdateLookupExecutor.kSBE;
                assert.eq(
                    delta.changeStreams.updateLookup.enrichBatchesStarted,
                    batchingApplies ? 2 : expectedChanges.length,
                    {delta},
                );

                const lookup = delta.changeStreams.updateLookup[engine];
                const fallbackLookup =
                    delta.changeStreams.updateLookup[UpdateLookupExecutor.kAggregation];

                // TODO: SERVER-134080 Support non-scalar _id lookups in SbeSingleDocumentLookupExecutor.
                // Only a non-clustered collection watched via SBE declines a compound _id (Express, the
                // db/cluster-level primary, handles it directly, as does SBE on a clustered collection).
                //
                // Every triplet here is [outerShape, middleShape, outerShape]: the two outer docs are
                // present, the middle one is gone. So there are only two present docs of outerShape
                // and one gone doc of middleShape to account for.
                const [outerShape, middleShape] = config.order;
                const outerDeclines = outerShape === "compound" && compoundDeclines;
                const middleDeclines = middleShape === "compound" && compoundDeclines;

                assert.eq(lookup.found, outerDeclines ? 0 : 2, {lookup});
                assert.eq(fallbackLookup.found, outerDeclines ? 2 : 0, {fallbackLookup});
                assert.eq(lookup.notFound, middleDeclines ? 0 : 1, {lookup});
                assert.eq(fallbackLookup.notFound, middleDeclines ? 1 : 0, {fallbackLookup});
                assert.eq(lookup.notHandled, (outerDeclines ? 2 : 0) + (middleDeclines ? 1 : 0), {
                    lookup,
                });
            });
        }
    });
});
