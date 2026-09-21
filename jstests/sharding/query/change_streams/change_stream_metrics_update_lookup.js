/**
 * Tests that when a DDL op relocates an update's post-image off its shard, the change stream's optimized
 * updateLookup primary declines and the aggregation fallback handles it, per the serverStatus metrics.
 * @tags: [
 *   requires_fcv_90,
 *   featureFlagChangeStreamOptimizedUpdateLookup,
 *   uses_change_streams,
 *   assumes_balancer_off,
 * ]
 */
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {
    expectedUpdateLookupEngine,
    readUpdateLookupDelta,
    ServerStatusMetrics,
    UpdateLookupExecutor,
} from "jstests/libs/query/change_stream_metrics_util.js";
import {
    withClusteredColl,
    withCollation,
    withShardedColl,
} from "jstests/libs/query/collection_config_decorators.js";
import {
    assertCollDataDistribution,
    ChangeStreamTest,
    ChangeStreamWatchMode,
    watchModeToString,
    withChangeStreamTest,
} from "jstests/libs/query/change_stream_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {assertCreateCollection} from "jstests/libs/collection_drop_recreate.js";

describe("sharded change stream updateLookup primary->fallback metrics", function () {
    let st;
    let mongosDB;

    // Periodic noops keep cluster time advancing so update events surface promptly. The feature
    // flag tag wires the optimized primary; it cannot be a fixture setParameter because older
    // binaries reject it at startup.
    before(function () {
        st = new ShardingTest({
            shards: 2,
            rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
        });

        mongosDB = st.s.getDB(jsTestName());
    });

    // Start each test from a pristine database whose primary is shard0.
    beforeEach(function () {
        assert.commandWorked(mongosDB.dropDatabase());
        assert.commandWorked(
            mongosDB.adminCommand({
                enableSharding: mongosDB.getName(),
                primaryShard: st.shard0.shardName,
            }),
        );
    });

    after(function () {
        st.stop();
    });

    // Runs 'body' under a updateLookup stream at 'watchMode' inside a serverStatus snapshot, then
    // asserts the primary declined once and the aggregation fallback handled it; 'expectFound'
    // picks which fallback outcome.
    function assertUpdateLookupViaAggregate({watchMode, coll, expectFound}, body) {
        const delta = ServerStatusMetrics.withServerStatusMetricsAcrossCluster(mongosDB, () => {
            withUpdateLookupStream(watchMode, coll, body);
        });
        const byEngine = readUpdateLookupDelta(delta);
        const primary = byEngine[expectedUpdateLookupEngine(watchMode)];
        const fallback = byEngine[UpdateLookupExecutor.kAggregation];
        assert.eq(primary.notHandled, 1, {byEngine, delta});
        assert.eq(fallback.found, expectFound ? 1 : 0, {byEngine, delta});
        assert.eq(fallback.notFound, expectFound ? 0 : 1, {byEngine, delta});
        // The fallback handled the lookup (found or notFound), so it recorded latency.
        assert.gt(fallback.latencyCount, 0, {byEngine, delta});
    }

    // Shard-key strategies for the relocation cases; every doc has a unique _id so writes match on
    // {_id: 0}, and 'shardKeyFilter' covers the shard key for moveChunk targeting.
    // shardCollection only accepts the simple collation, so collation is not a dimension for
    // these configs.
    const shardKeyConfigs = [
        {name: "_id", key: {_id: 1}, doc: {_id: 0}, shardKeyFilter: {_id: 0}},
        {name: "sk", key: {sk: 1}, doc: {_id: 0, sk: 0}, shardKeyFilter: {sk: 0}},
    ]
        .flatMap((config) => [config, withClusteredColl(config)])
        .flatMap(withShardedColl);

    // moveCollection/movePrimary target unsplittable/untracked collections, which do accept a
    // non-simple collation.
    const unshardedConfigs = [{name: "default", doc: {_id: 0}, collOpts: {}}]
        .flatMap((config) => [config, withClusteredColl(config)])
        .flatMap((config) => [config, withCollation(config)]);

    // Shards 'collName' on 'key'. Range keys start as one chunk on shard0; hashed keys are
    // presplit, so callers pin the doc's chunk with ensureStartsOnShard0().
    function shardedCollectionOnShard0(collName, key, collOpts) {
        const coll = mongosDB.getCollection(collName);
        assert.commandWorked(mongosDB.createCollection(collName, collOpts));
        const cmd = {shardCollection: coll.getFullName(), key};

        // Pin hashed sharding to a single chunk so the doc starts on shard0.
        if (Object.values(key).includes("hashed")) {
            cmd.numInitialChunks = 1;
        }
        assert.commandWorked(mongosDB.adminCommand(cmd));
        return coll;
    }

    // Hashed assignment is shuffled at creation and ignores numInitialChunks, so move the chunk
    // holding 'shardKeyFilter' to shard0 explicitly.
    function ensureStartsOnShard0(coll, shardKeyFilter) {
        assert.commandWorked(
            mongosDB.adminCommand({
                moveChunk: coll.getFullName(),
                find: shardKeyFilter,
                to: st.shard0.shardName,
            }),
        );
    }

    // Opens a updateLookup stream at 'watchMode' over 'coll' and runs 'fn(cst, cursor)'. Whole-db
    // and whole-cluster streams still surface events with the collection's own namespace, so
    // callers assert the same ns/documentKey at every level.
    function withUpdateLookupStream(watchMode, coll, fn) {
        // Cluster-level streams run against the admin database; the others against the test database.
        const csDb = ChangeStreamTest.getDBForChangeStream(watchMode, mongosDB);
        withChangeStreamTest(csDb, (cst) => {
            const cursor = cst.getChangeStream({
                watchMode,
                coll: coll.getName(),
                options: {fullDocument: "updateLookup"},
            });
            fn(cst, cursor);
        });
    }

    // Run every scenario at each watch level: the optimized primary differs by level (SBE for
    // collection-level, Express for whole-db / whole-cluster), and the sharded relocations also
    // run across every shard-key strategy.
    for (const watchMode of Object.values(ChangeStreamWatchMode)) {
        for (const config of shardKeyConfigs) {
            it(`moveChunk relocates the post-image [${watchModeToString(watchMode)}][${config.name}]: primary declines, aggregation finds it`, function () {
                const coll = shardedCollectionOnShard0("moveChunk", config.key, config.collOpts);
                assert.commandWorked(coll.insert(config.doc));
                ensureStartsOnShard0(coll, config.shardKeyFilter);

                assertUpdateLookupViaAggregate(
                    {watchMode, coll, expectFound: true},
                    (cst, cursor) => {
                        // Update while local, then move the chunk so the post-image is no longer on
                        // the shard observing the update.
                        assert.commandWorked(coll.update({_id: 0}, {$set: {v: 1}}));
                        assert.commandWorked(
                            mongosDB.adminCommand({
                                moveChunk: coll.getFullName(),
                                find: config.shardKeyFilter,
                                to: st.shard1.shardName,
                            }),
                        );
                        assertCollDataDistribution(mongosDB, coll, [
                            [st.shard0, 0],
                            [st.shard1, 1],
                        ]);

                        cst.assertNextChangesEqual({
                            cursor,
                            expectedChanges: [
                                {
                                    operationType: "update",
                                    ns: {db: mongosDB.getName(), coll: coll.getName()},
                                    documentKey: config.doc,
                                    fullDocument: {...config.doc, v: 1},
                                },
                            ],
                        });
                    },
                );
            });

            it(`reshardCollection relocates the post-image [${watchModeToString(watchMode)}][${config.name}]: primary declines, aggregation finds it`, function () {
                const coll = shardedCollectionOnShard0(
                    "reshardCollection",
                    config.key,
                    config.collOpts,
                );
                assert.commandWorked(coll.insert({...config.doc, rk: 0}));
                ensureStartsOnShard0(coll, config.shardKeyFilter);

                assertUpdateLookupViaAggregate(
                    {watchMode, coll, expectFound: true},
                    (cst, cursor) => {
                        assert.commandWorked(coll.update({_id: 0}, {$set: {v: 1}}));
                        // Reshard onto a new key with all data on shard1.
                        assert.commandWorked(
                            mongosDB.adminCommand({
                                reshardCollection: coll.getFullName(),
                                key: {rk: 1},
                                shardDistribution: [
                                    {
                                        shard: st.shard1.shardName,
                                        min: {rk: MinKey},
                                        max: {rk: MaxKey},
                                    },
                                ],
                            }),
                        );
                        assertCollDataDistribution(mongosDB, coll, [
                            [st.shard0, 0],
                            [st.shard1, 1],
                        ]);

                        cst.assertNextChangesEqual({
                            cursor,
                            expectedChanges: [
                                {
                                    operationType: "update",
                                    ns: {db: mongosDB.getName(), coll: coll.getName()},
                                    documentKey: config.doc,
                                    fullDocument: {...config.doc, rk: 0, v: 1},
                                },
                            ],
                        });
                    },
                );
            });

            it(`deleted post-image after relocation routes to aggregation.notFound [${watchModeToString(watchMode)}][${config.name}]`, function () {
                const coll = shardedCollectionOnShard0(
                    "deletedPostImage",
                    config.key,
                    config.collOpts,
                );
                assert.commandWorked(coll.insert(config.doc));
                ensureStartsOnShard0(coll, config.shardKeyFilter);

                assertUpdateLookupViaAggregate(
                    {watchMode, coll, expectFound: false},
                    (cst, cursor) => {
                        assert.commandWorked(coll.update({_id: 0}, {$set: {v: 1}}));
                        assert.commandWorked(
                            mongosDB.adminCommand({
                                moveChunk: coll.getFullName(),
                                find: config.shardKeyFilter,
                                to: st.shard1.shardName,
                            }),
                        );
                        assert.commandWorked(coll.remove({_id: 0}));
                        assertCollDataDistribution(mongosDB, coll, [
                            [st.shard0, 0],
                            [st.shard1, 0],
                        ]);

                        // The update's post-image lookup finds nothing; drain it and the delete.
                        const ns = {db: mongosDB.getName(), coll: coll.getName()};
                        cst.assertNextChangesEqual({
                            cursor,
                            expectedChanges: [
                                {
                                    operationType: "update",
                                    ns,
                                    documentKey: config.doc,
                                    fullDocument: null,
                                },
                                {operationType: "delete", ns, documentKey: config.doc},
                            ],
                        });
                    },
                );
            });

            // No relocation: the primary resolves the lookup locally.
            it(`updateLookup resolves the post-image locally without relocation [${watchModeToString(watchMode)}][${config.name}]`, function () {
                const coll = shardedCollectionOnShard0(
                    `noRelocation-${watchMode}-${config.name}`,
                    config.key,
                    config.collOpts,
                );
                assert.commandWorked(coll.insert(config.doc));

                const delta = ServerStatusMetrics.withServerStatusMetricsAcrossCluster(
                    mongosDB,
                    () => {
                        withUpdateLookupStream(watchMode, coll, (cst, cursor) => {
                            assert.commandWorked(coll.update({_id: 0}, {$set: {v: 1}}));

                            cst.assertNextChangesEqual({
                                cursor,
                                expectedChanges: [
                                    {
                                        operationType: "update",
                                        ns: {db: mongosDB.getName(), coll: coll.getName()},
                                        documentKey: config.doc,
                                        fullDocument: {...config.doc, v: 1},
                                    },
                                ],
                            });
                        });
                    },
                );

                const byEngine = readUpdateLookupDelta(delta);
                const primary = byEngine[expectedUpdateLookupEngine(watchMode)];
                assert.eq(primary.found, 1, {byEngine, delta});
                assert.eq(primary.notHandled, 0, {byEngine, delta});
                assert.eq(byEngine[UpdateLookupExecutor.kAggregation].found, 0, {byEngine, delta});
                assert.eq(byEngine[UpdateLookupExecutor.kAggregation].notFound, 0, {
                    byEngine,
                    delta,
                });
            });
        }

        // moveCollection/movePrimary operate on unsplittable/untracked collections, which (unlike
        // classic shardCollection) do accept a non-simple default collation, so they're the only
        // place in this suite that can exercise collation as a dimension.
        for (const config of unshardedConfigs) {
            it(`moveCollection relocates the post-image [${watchModeToString(watchMode)}][${config.name}]: primary declines, aggregation finds it`, function () {
                const coll = assertCreateCollection(mongosDB, "moveCollection", config.collOpts);
                assert.commandWorked(coll.insert(config.doc));

                assertUpdateLookupViaAggregate(
                    {watchMode, coll, expectFound: true},
                    (cst, cursor) => {
                        assert.commandWorked(coll.update(config.doc, {$set: {v: 1}}));
                        assert.commandWorked(
                            mongosDB.adminCommand({
                                moveCollection: coll.getFullName(),
                                toShard: st.shard1.shardName,
                            }),
                        );
                        assertCollDataDistribution(mongosDB, coll, [
                            [st.shard0, 0],
                            [st.shard1, 1],
                        ]);

                        cst.assertNextChangesEqual({
                            cursor,
                            expectedChanges: [
                                {
                                    operationType: "update",
                                    ns: {db: mongosDB.getName(), coll: coll.getName()},
                                    documentKey: config.doc,
                                    fullDocument: {...config.doc, v: 1},
                                },
                            ],
                        });
                    },
                );
            });

            it(`movePrimary relocates an untracked collection's post-image [${watchModeToString(watchMode)}][${config.name}]: primary declines, aggregation finds it`, function () {
                const coll = assertCreateCollection(mongosDB, "movePrimary", config.collOpts);
                assert.commandWorked(coll.insert(config.doc));

                assertUpdateLookupViaAggregate(
                    {watchMode, coll, expectFound: true},
                    (cst, cursor) => {
                        assert.commandWorked(coll.update(config.doc, {$set: {v: 1}}));
                        assert.commandWorked(
                            mongosDB.adminCommand({
                                movePrimary: mongosDB.getName(),
                                to: st.shard1.shardName,
                            }),
                        );
                        assertCollDataDistribution(mongosDB, coll, [
                            [st.shard0, 0],
                            [st.shard1, 1],
                        ]);

                        cst.assertNextChangesEqual({
                            cursor,
                            expectedChanges: [
                                {
                                    operationType: "update",
                                    ns: {db: mongosDB.getName(), coll: coll.getName()},
                                    documentKey: config.doc,
                                    fullDocument: {...config.doc, v: 1},
                                },
                            ],
                        });
                    },
                );
            });
        }
    }
});
