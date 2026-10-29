/**
 * Tests that when a DDL op relocates an update's post-image off its shard, the change stream's optimized
 * updateLookup primary declines and the aggregation fallback handles it, per the serverStatus metrics.
 * Also covers the no-relocation counterpart on unsplittable and untracked collections, where the
 * primary resolves the post-image with a local seek.
 * @tags: [
 *   requires_fcv_90,
 *   featureFlagChangeStreamOptimizedUpdateLookup,
 *   uses_change_streams,
 *   assumes_balancer_off,
 * ]
 */
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
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
    ChangeStreamWatchMode,
    changeStreamPassthroughType,
    watchModeToString,
    withChangeStreamTest,
} from "jstests/libs/query/change_stream_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {assertCreateCollection} from "jstests/libs/collection_drop_recreate.js";

describe("sharded change stream updateLookup primary->fallback metrics", function () {
    let st;
    let shardedDB;
    let unshardedDB;
    let untrackedDB;

    // Periodic noops keep cluster time advancing so update events surface promptly. The feature
    // flag tag wires the optimized primary; it cannot be a fixture setParameter because older
    // binaries reject it at startup.
    before(function () {
        st = new ShardingTest({
            shards: 2,
            rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
        });

        shardedDB = st.s.getDB(`${jsTestName()}_sharded`);
        unshardedDB = st.s.getDB(`${jsTestName()}_unsharded`);
        untrackedDB = st.s.getDB(`${jsTestName()}_untracked`);
    });

    // Start each test from pristine databases whose primary is shard0, and drop them again when
    // it is done.
    beforeEach(function () {
        for (const testDB of [shardedDB, unshardedDB]) {
            assert.commandWorked(
                testDB.adminCommand({
                    enableSharding: testDB.getName(),
                    primaryShard: st.shard0.shardName,
                }),
            );
        }
    });

    afterEach(function () {
        for (const testDB of [shardedDB, unshardedDB]) {
            assert.commandWorked(testDB.dropDatabase());
        }
    });

    after(function () {
        st.stop();
    });

    // Runs 'body' under a updateLookup stream inside a serverStatus snapshot, then asserts the
    // primary declined once and the aggregation fallback handled it; 'expectFound' picks which
    // fallback outcome.
    function assertUpdateLookupViaAggregate({coll, expectFound}, body) {
        const delta = ServerStatusMetrics.withServerStatusMetricsAcrossCluster(coll.getDB(), () => {
            withUpdateLookupStream(coll, body);
        });
        const byEngine = readUpdateLookupDelta(delta);
        const primary = byEngine[expectedUpdateLookupEngine()];
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
    // non-simple collation. The string _id makes the collation variants collation-sensitive: the
    // _id_ index entries (and, on a clustered collection, the RecordIds) are collation
    // comparison-key encoded, so a primary lookup that fails to adopt the collection's collator
    // seeks with the wrong key bytes and misses the document.
    const unshardedConfigs = [{name: "default", doc: {_id: "doc"}, collOpts: {}}]
        .flatMap((config) => [config, withClusteredColl(config)])
        .flatMap((config) => [config, withCollation(config)]);

    // Shards 'collName' on 'key'. Range keys start as one chunk on shard0; hashed keys are
    // presplit, so callers pin the doc's chunk with ensureStartsOnShard0().
    function shardedCollectionOnShard0(collName, key, collOpts) {
        const coll = shardedDB.getCollection(collName);
        assert.commandWorked(shardedDB.createCollection(collName, collOpts));
        const cmd = {shardCollection: coll.getFullName(), key};

        // Pin hashed sharding to a single chunk so the doc starts on shard0.
        if (Object.values(key).includes("hashed")) {
            cmd.numInitialChunks = 1;
        }
        assert.commandWorked(shardedDB.adminCommand(cmd));
        return coll;
    }

    // Hashed assignment is shuffled at creation and ignores numInitialChunks, so move the chunk
    // holding 'shardKeyFilter' to shard0 explicitly.
    function ensureStartsOnShard0(coll, shardKeyFilter) {
        assert.commandWorked(
            coll.getDB().adminCommand({
                moveChunk: coll.getFullName(),
                find: shardKeyFilter,
                to: st.shard0.shardName,
            }),
        );
    }

    // Opens a collection-level updateLookup stream over 'coll' and runs 'fn(cst, cursor)' on
    // 'coll''s own db.
    function withUpdateLookupStream(coll, fn) {
        withChangeStreamTest(coll.getDB(), (cst) => {
            const cursor = cst.getChangeStream({
                watchMode: ChangeStreamWatchMode.kCollection,
                coll: coll.getName(),
                options: {fullDocument: "updateLookup"},
            });
            fn(cst, cursor);
        });
    }

    // The optimized primary differs by level (SBE for collection-level, Express for whole-db /
    // whole-cluster); the sharded relocations also run across every shard-key strategy.
    const watchModeLabel = watchModeToString(changeStreamPassthroughType());
    for (const config of shardKeyConfigs) {
        it(`moveChunk relocates the post-image [${watchModeLabel}][${config.name}]: primary declines, aggregation finds it`, function () {
            const coll = shardedCollectionOnShard0("moveChunk", config.key, config.collOpts);
            assert.commandWorked(coll.insert(config.doc));
            ensureStartsOnShard0(coll, config.shardKeyFilter);

            assertUpdateLookupViaAggregate({coll, expectFound: true}, (cst, cursor) => {
                // Update while local, then move the chunk so the post-image is no longer on
                // the shard observing the update.
                assert.commandWorked(coll.update({_id: 0}, {$set: {v: 1}}));
                assert.commandWorked(
                    shardedDB.adminCommand({
                        moveChunk: coll.getFullName(),
                        find: config.shardKeyFilter,
                        to: st.shard1.shardName,
                    }),
                );
                assertCollDataDistribution(coll.getDB(), coll, [
                    [st.shard0, 0],
                    [st.shard1, 1],
                ]);

                cst.assertNextChangesEqual({
                    cursor,
                    expectedChanges: [
                        {
                            operationType: "update",
                            ns: {db: shardedDB.getName(), coll: coll.getName()},
                            documentKey: config.doc,
                            fullDocument: {...config.doc, v: 1},
                        },
                    ],
                });
            });
        });

        it(`reshardCollection relocates the post-image [${watchModeLabel}][${config.name}]: primary declines, aggregation finds it`, function () {
            const coll = shardedCollectionOnShard0(
                "reshardCollection",
                config.key,
                config.collOpts,
            );
            assert.commandWorked(coll.insert({...config.doc, rk: 0}));
            ensureStartsOnShard0(coll, config.shardKeyFilter);

            assertUpdateLookupViaAggregate({coll, expectFound: true}, (cst, cursor) => {
                assert.commandWorked(coll.update({_id: 0}, {$set: {v: 1}}));
                // Reshard onto a new key with all data on shard1.
                assert.commandWorked(
                    shardedDB.adminCommand({
                        reshardCollection: coll.getFullName(),
                        key: {rk: 1},
                        shardDistribution: [
                            {shard: st.shard1.shardName, min: {rk: MinKey}, max: {rk: MaxKey}},
                        ],
                    }),
                );
                assertCollDataDistribution(coll.getDB(), coll, [
                    [st.shard0, 0],
                    [st.shard1, 1],
                ]);

                cst.assertNextChangesEqual({
                    cursor,
                    expectedChanges: [
                        {
                            operationType: "update",
                            ns: {db: shardedDB.getName(), coll: coll.getName()},
                            documentKey: config.doc,
                            fullDocument: {...config.doc, rk: 0, v: 1},
                        },
                    ],
                });
            });
        });

        it(`deleted post-image after relocation routes to aggregation.notFound [${watchModeLabel}][${config.name}]`, function () {
            const coll = shardedCollectionOnShard0("deletedPostImage", config.key, config.collOpts);
            assert.commandWorked(coll.insert(config.doc));
            ensureStartsOnShard0(coll, config.shardKeyFilter);

            assertUpdateLookupViaAggregate({coll, expectFound: false}, (cst, cursor) => {
                assert.commandWorked(coll.update({_id: 0}, {$set: {v: 1}}));
                assert.commandWorked(
                    shardedDB.adminCommand({
                        moveChunk: coll.getFullName(),
                        find: config.shardKeyFilter,
                        to: st.shard1.shardName,
                    }),
                );
                assert.commandWorked(coll.remove({_id: 0}));
                assertCollDataDistribution(coll.getDB(), coll, [
                    [st.shard0, 0],
                    [st.shard1, 0],
                ]);

                // The update's post-image lookup finds nothing; drain it and the delete.
                const ns = {db: shardedDB.getName(), coll: coll.getName()};
                cst.assertNextChangesEqual({
                    cursor,
                    expectedChanges: [
                        {operationType: "update", ns, documentKey: config.doc, fullDocument: null},
                        {operationType: "delete", ns, documentKey: config.doc},
                    ],
                });
            });
        });

        // No relocation: the primary resolves the lookup locally.
        it(`updateLookup resolves the post-image locally without relocation [${watchModeLabel}][${config.name}]`, function () {
            const coll = shardedCollectionOnShard0(
                `noRelocation-${watchModeLabel}-${config.name}`,
                config.key,
                config.collOpts,
            );
            assert.commandWorked(coll.insert(config.doc));

            const delta = ServerStatusMetrics.withServerStatusMetricsAcrossCluster(
                coll.getDB(),
                () => {
                    withUpdateLookupStream(coll, (cst, cursor) => {
                        assert.commandWorked(coll.update({_id: 0}, {$set: {v: 1}}));

                        cst.assertNextChangesEqual({
                            cursor,
                            expectedChanges: [
                                {
                                    operationType: "update",
                                    ns: {db: shardedDB.getName(), coll: coll.getName()},
                                    documentKey: config.doc,
                                    fullDocument: {...config.doc, v: 1},
                                },
                            ],
                        });
                    });
                },
            );

            const byEngine = readUpdateLookupDelta(delta);
            const primary = byEngine[expectedUpdateLookupEngine()];
            assert.eq(primary.found, 1, {byEngine, delta});
            assert.eq(primary.notHandled, 0, {byEngine, delta});
            assert.eq(byEngine[UpdateLookupExecutor.kAggregation].found, 0, {byEngine, delta});
            assert.eq(byEngine[UpdateLookupExecutor.kAggregation].notFound, 0, {byEngine, delta});
        });
    }

    // The cases above each relocate a single document in isolation, so the primary executor
    // only ever declines once per drain. This interleaves a local doc, a relocated doc, and
    // another local doc within the same batch of change events, to stress that a decline
    // mid-batch doesn't corrupt the primary's cached plan/acquisition for the local lookups
    // that follow it. Scoped to range {_id: 1} sharding, since isolating one document's
    // chunk by exact value (rather than moving a whole shard-key range) doesn't generalize
    // cleanly to the hashed / non-_id shard-key configs above.
    it(`interleaved local/relocated/local docs in one batch [${watchModeLabel}]: primary handles the local docs, aggregation catches the relocated one`, function () {
        const coll = shardedCollectionOnShard0("interleavedRelocation", {_id: 1}, {});
        assert.commandWorked(coll.insert([{_id: 0}, {_id: 1}, {_id: 2}]));

        // The single pre-split chunk covering all three documents starts on shard0, the
        // same way every config above ensures.
        ensureStartsOnShard0(coll, {_id: 0});

        // Isolate _id:1 into its own chunk, distinct from _id:0 and _id:2, so only its data
        // can be relocated without dragging the other two documents along with it.
        assert.commandWorked(shardedDB.adminCommand({split: coll.getFullName(), middle: {_id: 1}}));
        assert.commandWorked(shardedDB.adminCommand({split: coll.getFullName(), middle: {_id: 2}}));

        const delta = ServerStatusMetrics.withServerStatusMetricsAcrossCluster(coll.getDB(), () => {
            withUpdateLookupStream(coll, (cst, cursor) => {
                // Record all three updates on shard0's oplog while every document is still
                // local, then relocate only _id:1's chunk, so in interleaved order the first
                // and third post-image lookups resolve locally via the primary and the
                // second is forced through the aggregation fallback.
                assert.commandWorked(coll.update({_id: 0}, {$set: {v: 1}}));
                assert.commandWorked(coll.update({_id: 1}, {$set: {v: 1}}));
                assert.commandWorked(coll.update({_id: 2}, {$set: {v: 1}}));
                assert.commandWorked(
                    shardedDB.adminCommand({
                        moveChunk: coll.getFullName(),
                        find: {_id: 1},
                        to: st.shard1.shardName,
                    }),
                );
                assertCollDataDistribution(coll.getDB(), coll, [
                    [st.shard0, 2],
                    [st.shard1, 1],
                ]);

                const ns = {db: shardedDB.getName(), coll: coll.getName()};
                cst.assertNextChangesEqual({
                    cursor,
                    expectedChanges: [
                        {
                            operationType: "update",
                            ns,
                            documentKey: {_id: 0},
                            fullDocument: {_id: 0, v: 1},
                        },
                        {
                            operationType: "update",
                            ns,
                            documentKey: {_id: 1},
                            fullDocument: {_id: 1, v: 1},
                        },
                        {
                            operationType: "update",
                            ns,
                            documentKey: {_id: 2},
                            fullDocument: {_id: 2, v: 1},
                        },
                    ],
                });
            });
        });

        const byEngine = readUpdateLookupDelta(delta);
        const primary = byEngine[expectedUpdateLookupEngine()];
        // The two local docs resolve directly through the primary; the relocated one is
        // declined by the primary and found instead by the aggregation fallback.
        assert.eq(primary.found, 2, {byEngine, delta});
        assert.eq(primary.notHandled, 1, {byEngine, delta});
        assert.eq(byEngine[UpdateLookupExecutor.kAggregation].found, 1, {byEngine, delta});
        assert.eq(byEngine[UpdateLookupExecutor.kAggregation].notFound, 0, {byEngine, delta});
    });

    // moveCollection/movePrimary operate on unsplittable/untracked collections, which (unlike
    // classic shardCollection) do accept a non-simple default collation, so they're the only
    // configs in this suite that carry collation.
    for (const config of unshardedConfigs) {
        it(`moveCollection relocates the post-image [${watchModeLabel}][${config.name}]: primary declines, aggregation finds it`, function () {
            const coll = assertCreateCollection(unshardedDB, "moveCollection", config.collOpts);
            assert.commandWorked(coll.insert(config.doc));

            assertUpdateLookupViaAggregate({coll, expectFound: true}, (cst, cursor) => {
                assert.commandWorked(coll.update(config.doc, {$set: {v: 1}}));
                assert.commandWorked(
                    unshardedDB.adminCommand({
                        moveCollection: coll.getFullName(),
                        toShard: st.shard1.shardName,
                    }),
                );
                assertCollDataDistribution(coll.getDB(), coll, [
                    [st.shard0, 0],
                    [st.shard1, 1],
                ]);

                cst.assertNextChangesEqual({
                    cursor,
                    expectedChanges: [
                        {
                            operationType: "update",
                            ns: {db: unshardedDB.getName(), coll: coll.getName()},
                            documentKey: config.doc,
                            fullDocument: {...config.doc, v: 1},
                        },
                    ],
                });
            });
        });

        it(`movePrimary relocates an unsharded collection's post-image [${watchModeLabel}][${config.name}]: primary declines, aggregation finds it`, function () {
            const coll = assertCreateCollection(unshardedDB, "movePrimary", config.collOpts);
            assert.commandWorked(coll.insert(config.doc));

            assertUpdateLookupViaAggregate({coll, expectFound: true}, (cst, cursor) => {
                assert.commandWorked(coll.update(config.doc, {$set: {v: 1}}));
                assert.commandWorked(
                    unshardedDB.adminCommand({
                        movePrimary: unshardedDB.getName(),
                        to: st.shard1.shardName,
                    }),
                );
                assertCollDataDistribution(coll.getDB(), coll, [
                    [st.shard0, 0],
                    [st.shard1, 1],
                ]);

                cst.assertNextChangesEqual({
                    cursor,
                    expectedChanges: [
                        {
                            operationType: "update",
                            ns: {db: unshardedDB.getName(), coll: coll.getName()},
                            documentKey: config.doc,
                            fullDocument: {...config.doc, v: 1},
                        },
                    ],
                });
            });
        });

        for (const placement of [
            {name: "unsplittable", untracked: false},
            {name: "untracked", untracked: true},
        ]) {
            it(`updateLookup resolves the post-image locally without relocation [${watchModeLabel}][${config.name}][${placement.name}]`, function () {
                const collName = `noRelocation${placement.untracked ? "Untracked" : "Unsplittable"}`;
                let coll;
                if (placement.untracked) {
                    // Unique per config: untrackedDB persists across the configs (never dropped).
                    const untrackedCollName = `${collName}_${config.name.replace(/[^a-zA-Z0-9]/g, "_")}`;
                    assert.commandWorked(
                        untrackedDB.createCollection(untrackedCollName, config.collOpts),
                    );
                    coll = untrackedDB.getCollection(untrackedCollName);
                } else {
                    assert.commandWorked(
                        unshardedDB.runCommand({
                            createUnsplittableCollection: collName,
                            dataShard: st.shard0.shardName,
                            ...config.collOpts,
                        }),
                    );
                    coll = unshardedDB.getCollection(collName);
                }
                assert.commandWorked(coll.insert(config.doc));

                const delta = ServerStatusMetrics.withServerStatusMetricsAcrossCluster(
                    coll.getDB(),
                    () => {
                        withUpdateLookupStream(coll, (cst, cursor) => {
                            assert.commandWorked(
                                coll.update({_id: config.doc._id}, {$set: {v: 1}}),
                            );

                            cst.assertNextChangesEqual({
                                cursor,
                                expectedChanges: [
                                    {
                                        operationType: "update",
                                        ns: {db: coll.getDB().getName(), coll: coll.getName()},
                                        documentKey: config.doc,
                                        fullDocument: {...config.doc, v: 1},
                                    },
                                ],
                            });
                        });
                    },
                );

                const byEngine = readUpdateLookupDelta(delta);
                const primary = byEngine[expectedUpdateLookupEngine()];
                assert.eq(primary.found, 1, {byEngine, delta});
                assert.eq(primary.notHandled, 0, {byEngine, delta});
                assert.eq(byEngine[UpdateLookupExecutor.kAggregation].found, 0, {byEngine, delta});
                assert.eq(byEngine[UpdateLookupExecutor.kAggregation].notFound, 0, {
                    byEngine,
                    delta,
                });
            });
        }
    }
});
