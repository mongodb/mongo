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
import {ServerStatusMetrics} from "jstests/libs/query/change_stream_metrics_util.js";
import {
    assertCollDataDistribution,
    ChangeStreamTest,
    ChangeStreamWatchMode,
    watchModeToString,
    withChangeStreamTest,
} from "jstests/libs/query/change_stream_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("sharded change stream updateLookup primary->fallback metrics", function () {
    let st;
    let mongosDB;

    // Higher-frequency periodic noops keep the change stream's cluster time advancing so update
    // events become available promptly. The 'featureFlagChangeStreamOptimizedUpdateLookup' tag
    // ensures the optimized primary (and thus the primary->fallback handoff under test) is wired;
    // it must not be set as a fixture --setParameter, which older binaries reject at startup.
    before(function () {
        st = new ShardingTest({
            shards: 2,
            rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
        });

        mongosDB = st.s.getDB(jsTestName());
    });

    // Start each test from a pristine database whose primary is shard0. Relocations performed by a
    // test are reset before the next test, so no case needs its own isolated database.
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

    // Reads the updateLookup single-document-lookup deltas out of a serverStatus metrics diff,
    // treating any missing leaf as 0. The primary is whichever optimized engine is wired today
    // (Express, with SBE reserved for a follow-up), so we sum express + sbe notHandled to be
    // agnostic to which primary declined.
    function readUpdateLookupDelta(delta) {
        const ul = (delta.changeStreams && delta.changeStreams.updateLookup) || {};
        const leaf = (engine, field) => (ul[engine] && ul[engine][field]) || 0;
        return {
            primaryNotHandled: leaf("express", "notHandled") + leaf("sbe", "notHandled"),
            aggFound: leaf("aggregation", "found"),
            aggNotFound: leaf("aggregation", "notFound"),
            // 'latencyMicros' is a histogram; 'totalCount' is its number of recorded observations.
            aggLatencyCount:
                (ul.aggregation &&
                    ul.aggregation.latencyMicros &&
                    ul.aggregation.latencyMicros.totalCount) ||
                0,
        };
    }

    // Opens a 'fullDocument: updateLookup' change stream at 'watchMode' over 'coll', runs
    // 'body(cst, cursor)' (which updates the doc, relocates it, and drains) while snapshotting the
    // cluster-wide serverStatus metrics, then asserts the primary declined once and the aggregation
    // fallback handled it. The fallback's two outcomes are mutually exclusive, so a single
    // 'expectFound' flag suffices: true means it should have found the post-image, false means it
    // should have found it absent.
    function assertUpdateLookupViaAggregate({watchMode, coll, expectFound}, body) {
        const delta = ServerStatusMetrics.withServerStatusMetricsAcrossCluster(mongosDB, () => {
            withUpdateLookupStream(watchMode, coll, body);
        });
        const observed = readUpdateLookupDelta(delta);
        assert.eq(observed.primaryNotHandled, 1, {observed, delta});
        assert.eq(observed.aggFound, expectFound ? 1 : 0, {observed, delta});
        assert.eq(observed.aggNotFound, expectFound ? 0 : 1, {observed, delta});
        // The aggregation fallback handled the lookup (found or notFound), so it recorded latency.
        assert.gt(observed.aggLatencyCount, 0, {observed, delta});
    }

    // Shard-key strategies exercised by the sharded relocation cases, to confirm the
    // primary->fallback handoff is independent of how the collection is sharded. Every doc has a
    // unique _id, so updates/removes can always match on {_id: 0}; 'shardKeyFilter' covers the shard
    // key for moveChunk targeting, and 'doc' carries exactly the shard-key fields.
    const shardKeyConfigs = [
        {name: "hashed _id", key: {_id: "hashed"}, doc: {_id: 0}, shardKeyFilter: {_id: 0}},
        {name: "range _id", key: {_id: 1}, doc: {_id: 0}, shardKeyFilter: {_id: 0}},
        {name: "range sk", key: {sk: 1}, doc: {_id: 0, sk: 0}, shardKeyFilter: {sk: 0}},
    ];

    // Shards 'collName' on 'key' as a single chunk on shard0 and returns the collection.
    function shardedCollectionOnShard0(collName, key) {
        const coll = mongosDB.getCollection(collName);
        const cmd = {shardCollection: coll.getFullName(), key};
        // Hashed sharding otherwise pre-splits into several chunks spread across all shards; pin it
        // to a single chunk so the doc starts on shard0 (range keys already start as one chunk).
        if (Object.values(key).includes("hashed")) {
            cmd.numInitialChunks = 1;
        }
        assert.commandWorked(mongosDB.adminCommand(cmd));
        return coll;
    }

    // Asserts (retrying, so the donor's asynchronous range deletion / cleanup can finish) that the
    // collection's single document has fully relocated to shard1, with nothing left on shard0.
    function assertDocRelocatedToShard1(coll) {
        assertCollDataDistribution(mongosDB, coll, [
            [st.shard0, 0],
            [st.shard1, 1],
        ]);
    }

    // Opens a 'fullDocument: updateLookup' change stream at the given watch level over 'coll', runs
    // 'fn(cst, cursor)', then cleans up. getChangeStream() handles the watchMode-specific stage
    // options and collection selection; whole-db / whole-cluster streams still surface events with
    // the collection's own namespace, so callers assert the same ns/documentKey at every level.
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

    // Every scenario runs at each watch level; the sharded relocations additionally run across every
    // shard-key strategy so the handoff is verified across all sharding shapes. The optimized
    // primary differs by change-stream level (SBE for collection-level streams, Express for
    // whole-db / whole-cluster), so covering all levels exercises whichever primary is selected.
    for (const watchMode of Object.values(ChangeStreamWatchMode)) {
        for (const config of shardKeyConfigs) {
            it(`moveChunk relocates the post-image [${watchModeToString(watchMode)}][${config.name}]: primary declines, aggregation finds it`, function () {
                const coll = shardedCollectionOnShard0("moveChunk", config.key);
                assert.commandWorked(coll.insert(config.doc));

                assertUpdateLookupViaAggregate(
                    {watchMode, coll, expectFound: true},
                    (cst, cursor) => {
                        // Record the update on shard0's oplog, then move the doc's chunk to shard1 so
                        // the post-image is no longer local to the shard observing the update.
                        assert.commandWorked(coll.update({_id: 0}, {$set: {v: 1}}));
                        assert.commandWorked(
                            mongosDB.adminCommand({
                                moveChunk: coll.getFullName(),
                                find: config.shardKeyFilter,
                                to: st.shard1.shardName,
                            }),
                        );
                        assertDocRelocatedToShard1(coll);

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
                const coll = shardedCollectionOnShard0("reshardCollection", config.key);
                assert.commandWorked(coll.insert({...config.doc, rk: 0}));

                assertUpdateLookupViaAggregate(
                    {watchMode, coll, expectFound: true},
                    (cst, cursor) => {
                        assert.commandWorked(coll.update({_id: 0}, {$set: {v: 1}}));
                        // Reshard onto a new key with all data placed on shard1, changing the
                        // collection's UUID and relocating the post-image off shard0.
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
                        assertDocRelocatedToShard1(coll);

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
                const coll = shardedCollectionOnShard0("deletedPostImage", config.key);
                assert.commandWorked(coll.insert(config.doc));

                assertUpdateLookupViaAggregate(
                    {watchMode, coll, expectFound: false},
                    (cst, cursor) => {
                        // Record the update on shard0, relocate the doc off shard0 so the primary
                        // declines, then delete it so the aggregation fallback finds nothing.
                        assert.commandWorked(coll.update({_id: 0}, {$set: {v: 1}}));
                        assert.commandWorked(
                            mongosDB.adminCommand({
                                moveChunk: coll.getFullName(),
                                find: config.shardKeyFilter,
                                to: st.shard1.shardName,
                            }),
                        );
                        assert.commandWorked(coll.remove({_id: 0}));
                        // The relocated doc is now deleted, so neither shard holds it.
                        assertCollDataDistribution(mongosDB, coll, [
                            [st.shard0, 0],
                            [st.shard1, 0],
                        ]);

                        // Drain the update (whose post-image lookup now finds nothing, so fullDocument
                        // resolves to null) and the following delete.
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
        }

        it(`moveCollection relocates the post-image [${watchModeToString(watchMode)}]: primary declines, aggregation finds it`, function () {
            // moveCollection operates on an unsharded collection, which lives entirely on its owning
            // shard (shard0 here). Create it by inserting.
            const coll = mongosDB.getCollection("moveCollection");
            assert.commandWorked(coll.insert({_id: 0}));

            assertUpdateLookupViaAggregate({watchMode, coll, expectFound: true}, (cst, cursor) => {
                assert.commandWorked(coll.update({_id: 0}, {$set: {v: 1}}));
                assert.commandWorked(
                    mongosDB.adminCommand({
                        moveCollection: coll.getFullName(),
                        toShard: st.shard1.shardName,
                    }),
                );
                assertDocRelocatedToShard1(coll);

                cst.assertNextChangesEqual({
                    cursor,
                    expectedChanges: [
                        {
                            operationType: "update",
                            ns: {db: mongosDB.getName(), coll: coll.getName()},
                            documentKey: {_id: 0},
                            fullDocument: {_id: 0, v: 1},
                        },
                    ],
                });
            });
        });

        it(`movePrimary relocates an untracked collection's post-image [${watchModeToString(watchMode)}]: primary declines, aggregation finds it`, function () {
            // Unlike moveCollection (a resharding-based single-collection move), movePrimary
            // relocates every untracked collection of a database along with its primary shard,
            // exercising a distinct code path. beforeEach restores this database's primary to shard0.
            const coll = mongosDB.getCollection("movePrimary");
            assert.commandWorked(coll.insert({_id: 0}));

            assertUpdateLookupViaAggregate({watchMode, coll, expectFound: true}, (cst, cursor) => {
                assert.commandWorked(coll.update({_id: 0}, {$set: {v: 1}}));
                assert.commandWorked(
                    mongosDB.adminCommand({
                        movePrimary: mongosDB.getName(),
                        to: st.shard1.shardName,
                    }),
                );
                assertDocRelocatedToShard1(coll);

                // Assert only the update event; on an untracked collection movePrimary also emits a
                // trailing drop (+ invalidate for collection-level streams), but the update whose
                // post-image lookup we care about precedes them.
                cst.assertNextChangesEqual({
                    cursor,
                    expectedChanges: [
                        {
                            operationType: "update",
                            ns: {db: mongosDB.getName(), coll: coll.getName()},
                            documentKey: {_id: 0},
                            fullDocument: {_id: 0, v: 1},
                        },
                    ],
                });
            });
        });
    }
});
