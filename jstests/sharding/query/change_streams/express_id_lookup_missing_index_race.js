/**
 * Coverage for an express _id-lookup plan whose _id index vanishes between opening the plan and a
 * later restoreState() re-fetch on the same NSS/UUID/catalog epoch. No user-facing DDL produces
 * this; the tests below manufacture it via an applyOps dropIndexes gated by the test-only
 * allowDroppingIdIndexForTesting failpoint.
 *
 * A plain aggregate hits that race directly and must fail with QueryPlanKilled, never crash the
 * server.
 *
 * A change stream updateLookup never hits that race (its lookup executors don't save/restore
 * mid-lookup); instead, when the _id index is missing at lookup time, the primary strategy
 * declines and the pre-existing aggregation-based fallback resolves it via a normal collection
 * scan. That holds regardless of the optimization flag and regardless of whether the lookup is
 * local or remote, so updateLookup always succeeds here rather than failing.
 *
 * Runs on a sharded cluster (not a replica set) so the same tests also cover a *remote* lookup:
 * the moveChunk case relocates the post-image off the shard that observed the update before the
 * lookup runs.
 *
 * @tags: [
 *   requires_fcv_90,
 *   # The suite-level 'clusterAllCollectionsByDefault' failpoint forces every collection to be
 *   # clustered, which leaves no separate _id index for the tests below to drop.
 *   expects_explicit_underscore_id_index,
 * ]
 */
import {
    assertCreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {withFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {
    ChangeStreamTest,
    changeStreamPassthroughType,
    watchModeToString,
    withChangeStreamTest,
} from "jstests/libs/query/change_stream_util.js";
import {
    expectedUpdateLookupEngine,
    readUpdateLookupDelta,
    ServerStatusMetrics,
    UpdateLookupExecutor,
} from "jstests/libs/query/change_stream_metrics_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

describe("express _id lookup across a vanished _id index", function () {
    let st;
    let db;
    const collName = "lookup_target";
    const watchMode = changeStreamPassthroughType();

    before(function () {
        // Periodic noops keep cluster time advancing so update events surface promptly.
        st = new ShardingTest({
            shards: 2,
            rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
        });
        db = st.s.getDB(jsTestName());
        assert.commandWorked(
            db.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}),
        );
    });

    beforeEach(function () {
        assertCreateCollection(db, collName);
        assert.commandWorked(db[collName].insert({_id: 1, x: 1}));
    });

    afterEach(function () {
        assertDropCollection(db, collName);
    });

    after(function () {
        st.stop();
    });

    // Reproduces the buggy state on 'conn' by dropping _id index.
    function dropIdIndexViaApplyOps(conn, dbName, coll) {
        withFailPoint(conn, "allowDroppingIdIndexForTesting", () => {
            assert.commandWorked(
                conn.adminCommand({
                    applyOps: [
                        {op: "c", ns: `${dbName}.$cmd`, o: {dropIndexes: coll, index: "_id_"}},
                    ],
                }),
            );
        });
    }

    // Asserts the updateLookup actually fell back to AggregationSingleDocumentLookupExecutor. 'primaryEngine' is the SBE/Express engine expected to have
    // declined, or null when the optimization is off, in which
    // case no primary is ever constructed and neither engine's counters move at all.
    function assertFellBackToAggregation(delta, primaryEngine) {
        for (const engine of [UpdateLookupExecutor.kSBE, UpdateLookupExecutor.kExpress]) {
            if (engine === primaryEngine) {
                assert.eq(delta[engine].notHandled, 1, {delta, engine});
                assert.eq(delta[engine].found, 0, {delta, engine});
            } else {
                assert.eq(delta[engine].notHandled, 0, {delta, engine});
                assert.eq(delta[engine].found, 0, {delta, engine});
            }
        }
        const fallback = delta[UpdateLookupExecutor.kAggregation];
        assert.eq(fallback.found, 1, {delta});
        assert.gt(fallback.latencyCount, 0, {delta});
    }

    it("fails an aggregate with QueryPlanKilled", function () {
        const shard0 = st.rs0.getPrimary();
        const fullNs = `${db.getName()}.${collName}`;

        withFailPoint(
            shard0,
            "hangBeforeDocumentSourceCursorLoadBatch",
            (fpLoadBatch) => {
                const awaitAggregate = startParallelShell(
                    funWithArgs(
                        function (dbName, collName) {
                            // $_internalInhibitOptimization forces a real DocumentSourceCursor.
                            assert.commandFailedWithCode(
                                db.getSiblingDB(dbName).runCommand({
                                    aggregate: collName,
                                    pipeline: [
                                        {$match: {_id: 1}},
                                        {$_internalInhibitOptimization: {}},
                                    ],
                                    cursor: {},
                                }),
                                ErrorCodes.QueryPlanKilled,
                            );
                        },
                        db.getName(),
                        collName,
                    ),
                    st.s.port,
                );
                fpLoadBatch.wait();

                dropIdIndexViaApplyOps(shard0, db.getName(), collName);

                fpLoadBatch.off();
                awaitAggregate();
            },
            {data: {nss: fullNs}},
        );
    });

    for (const featureFlagChangeStreamOptimizedUpdateLookup of [true, false]) {
        it(`updateLookup succeeds via the aggregation fallback when the _id index is missing [${watchModeToString(watchMode)} watch, optimization ${featureFlagChangeStreamOptimizedUpdateLookup}]`, function () {
            const shard0 = st.rs0.getPrimary();

            runWithParamsAllNonConfigNodes(
                db,
                {featureFlagChangeStreamOptimizedUpdateLookup},
                () => {
                    withChangeStreamTest(
                        ChangeStreamTest.getDBForChangeStream(watchMode, db),
                        (cst) => {
                            const cursor = cst.getChangeStream({
                                watchMode,
                                coll: collName,
                                options: {fullDocument: "updateLookup"},
                            });
                            assert.commandWorked(db[collName].update({_id: 1}, {$set: {x: 2}}));

                            dropIdIndexViaApplyOps(shard0, db.getName(), collName);

                            const delta = ServerStatusMetrics.withServerStatusMetricsAcrossCluster(
                                db,
                                () => {
                                    cst.assertNextChangesEqual({
                                        cursor,
                                        expectedChanges: [
                                            {
                                                operationType: "update",
                                                ns: {db: db.getName(), coll: collName},
                                                documentKey: {_id: 1},
                                                fullDocument: {_id: 1, x: 2},
                                            },
                                        ],
                                    });
                                },
                            );
                            assertFellBackToAggregation(
                                readUpdateLookupDelta(delta),
                                featureFlagChangeStreamOptimizedUpdateLookup
                                    ? expectedUpdateLookupEngine(watchMode)
                                    : null,
                            );
                        },
                    );
                },
            );
        });
    }

    // The remote-lookup case: the post-image relocates to a different shard than the one that
    // observed the update, before the lookup runs. Same fallback-to-collscan success, just via
    // a cross-shard route. Reuses 'collName' (sharded in-place) so afterEach's drop covers it.
    it(`updateLookup succeeds via the aggregation fallback on a relocated post-image [${watchModeToString(watchMode)} watch]`, function () {
        const fullNs = `${db.getName()}.${collName}`;
        assert.commandWorked(db.adminCommand({shardCollection: fullNs, key: {_id: 1}}));

        withChangeStreamTest(ChangeStreamTest.getDBForChangeStream(watchMode, db), (cst) => {
            const cursor = cst.getChangeStream({
                watchMode,
                coll: collName,
                options: {fullDocument: "updateLookup"},
            });

            // Update while the doc is still on shard0, then move it off before the lookup
            // runs: the lookup must be remote by the time it executes.
            assert.commandWorked(db[collName].update({_id: 1}, {$set: {x: 2}}));
            assert.commandWorked(
                db.adminCommand({moveChunk: fullNs, find: {_id: 1}, to: st.shard1.shardName}),
            );

            dropIdIndexViaApplyOps(st.rs1.getPrimary(), db.getName(), collName);

            const delta = ServerStatusMetrics.withServerStatusMetricsAcrossCluster(db, () => {
                cst.assertNextChangesEqual({
                    cursor,
                    expectedChanges: [
                        {
                            operationType: "update",
                            ns: {db: db.getName(), coll: collName},
                            documentKey: {_id: 1},
                            fullDocument: {_id: 1, x: 2},
                        },
                    ],
                });
            });
            assertFellBackToAggregation(
                readUpdateLookupDelta(delta),
                expectedUpdateLookupEngine(watchMode),
            );
        });
    });
});
