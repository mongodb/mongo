/**
 * Verifies that enriching update events with a post-image (fullDocument: "updateLookup") records the
 * single-document-lookup outcome into the per-engine serverStatus metrics under
 * 'changeStreams.updateLookup.<engine>'.
 *
 * @tags: [
 *   # Can not run in balancer suites as it expects no data migrations across shards for correct
 *   # metric capture (test expects shard local lookup).
 *   assumes_balancer_off,
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
import {
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {
    ChangeStreamWatchMode,
    changeStreamPassthroughType,
    withChangeStreamTest,
} from "jstests/libs/query/change_stream_util.js";
import {
    ServerStatusMetrics,
    UpdateLookupExecutor,
} from "jstests/libs/query/change_stream_metrics_util.js";

// The engine expected to handle an updateLookup given the optimized-updateLookup flag state. When
// the flag is off, the Aggregation executor is the entire lookup path regardless of topology. When
// it's on: collection-level streams have a fixed lookup namespace and use the caching SBE executor;
// db/cluster-level streams look up a different namespace per event and use Express.
function expectedEngine(isRunningOptimizedUpdateLookup) {
    if (!isRunningOptimizedUpdateLookup) {
        return UpdateLookupExecutor.kAggregation;
    }
    return changeStreamPassthroughType() === ChangeStreamWatchMode.kCollection
        ? UpdateLookupExecutor.kSBE
        : UpdateLookupExecutor.kExpress;
}

describe("change stream updateLookup single-document-lookup metrics", function () {
    function compoundId(seed) {
        return {nsUUID: UUID(), ts: Timestamp(seed, 1), applyOpsIndex: 0};
    }

    const configs = [
        {name: "unclustered", collOpts: {}, presentId: "present", goneId: "gone"},
        {
            name: "unclustered collation",
            collOpts: {collation: {locale: "en", strength: 2}},
            presentId: "present",
            goneId: "gone",
        },
        {
            name: "clustered scalar _id",
            collOpts: {clusteredIndex: {key: {_id: 1}, unique: true}},
            presentId: 1,
            goneId: 2,
        },
        {
            name: "clustered compound _id",
            collOpts: {clusteredIndex: {key: {_id: 1}, unique: true}},
            presentId: compoundId(100),
            goneId: compoundId(200),
        },
        {
            name: "clustered collation",
            collOpts: {
                clusteredIndex: {key: {_id: 1}, unique: true},
                collation: {locale: "en", strength: 2},
            },
            presentId: "present",
            goneId: "gone",
        },
    ];

    for (const config of configs) {
        describe(config.name, function () {
            const testDB = db.getSiblingDB(jsTestName() + "_" + config.name.replace(/\s+/g, "_"));
            const testColl = testDB.getCollection("test");
            const ns = {db: testDB.getName(), coll: testColl.getName()};
            let isRunningOptimizedUpdateLookup;

            before(function () {
                isRunningOptimizedUpdateLookup = FeatureFlagUtil.isEnabled(
                    testDB,
                    "ChangeStreamOptimizedUpdateLookup",
                );
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
                const engine = expectedEngine(isRunningOptimizedUpdateLookup);

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

                            // Assert on the actual event content, not just metric counts.
                            // cst.assertNextChangesEqualWithDeploymentAwareness also drains all 3
                            // events (2 updates + 1 delete) so the server has completed both
                            // post-image lookups before we read serverStatus below.
                            // Sharded topologies don't guarantee cross-shard event order matches
                            // client issue order (e.g. a transaction-wrapping passthrough can
                            // retry one op past a sibling op's commit on StaleConfig), so this
                            // compares the batch of events as an unordered set there.
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
                assert.eq(lookup.found, 1, {lookup});
                assert.eq(lookup.notFound, 1, {lookup});

                // Since there are no migrations, the primary executor should always succeed.
                assert.eq(lookup.notHandled, 0, {lookup});

                // Both lookups (found + notFound) recorded a latency observation.
                // 'latencyMicros' is a histogram; 'totalCount' is its number of recorded
                // observations.
                assert.gt(lookup.latencyMicros.totalCount, 0, {lookup});
            });
        });
    }
});
