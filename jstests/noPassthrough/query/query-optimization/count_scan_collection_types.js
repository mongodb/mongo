/**
 * Tests that the unfiltered-count COUNT_SCAN optimization behaves correctly across collection types
 * where the optimization does not apply, not because it would generate an incorrect plan, but
 * because it is not relevant or beneficial for those collection types:
 *   - timeseries collections: the optimization cannot apply (count pipelines over timeseries are
 *     never planned as count-like, and the buckets collection has no _id index); counts must
 *     return the number of measurements, never the number of buckets.
 *   - clustered collections: counting via the clustered _id index reads the whole collection
 *     anyway (the index and the data are the same tree), so a COUNT_SCAN has no benefit over a
 *     CLUSTERED_IXSCAN/COLLSCAN.
 *   - sharded collections: counting must filter out orphaned documents, which requires reading
 *     full documents, so the plan keeps shard filtering and intentionally gives up the
 *     COUNT_SCAN stage.
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    getPlanStage,
    getPlanStages,
    getWinningPlanFromExplain,
} from "jstests/libs/query/analyze_plan.js";

const kPipelines = [[{$count: "n"}], [{$group: {_id: 1, n: {$sum: 1}}}]];

function hasCountScan(explainRes) {
    return getPlanStage(getWinningPlanFromExplain(explainRes), "COUNT_SCAN") !== null;
}

describe("unfiltered count planning across collection types", function () {
    let conn;
    let db;

    before(function () {
        conn = MongoRunner.runMongod({});
        db = conn.getDB("test");
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    describe("timeseries collections", function () {
        const kMeasurementCount = 100;
        let coll;

        before(function () {
            coll = db.ts_count_scan;
            assert.commandWorked(
                db.createCollection(coll.getName(), {
                    timeseries: {timeField: "t", metaField: "m"},
                }),
            );
            assert.commandWorked(
                coll.insertMany(
                    Array.from({length: kMeasurementCount}, (_, i) => ({
                        t: new Date(2026, 0, 1 + (i % 28), i % 24),
                        m: i % 5,
                    })),
                ),
            );
        });

        after(function () {
            coll.drop();
        });

        it("counts measurements, not buckets, and never plans a COUNT_SCAN", function () {
            for (const pipeline of kPipelines) {
                assert.eq(kMeasurementCount, coll.aggregate(pipeline).toArray()[0].n);
                assert(
                    !hasCountScan(coll.explain().aggregate(pipeline)),
                    "did not expect a COUNT_SCAN for a timeseries collection",
                );
            }
            assert.eq(kMeasurementCount, coll.count());
        });
    });

    describe("clustered collections", function () {
        const kDocCount = 50;
        let coll;

        before(function () {
            coll = db.clustered_count_scan;
            assert.commandWorked(
                db.createCollection(coll.getName(), {
                    clusteredIndex: {key: {_id: 1}, unique: true},
                }),
            );
            assert.commandWorked(
                coll.insertMany(Array.from({length: kDocCount}, (_, i) => ({_id: i}))),
            );
        });

        after(function () {
            coll.drop();
        });

        it("counts correctly without a COUNT_SCAN", function () {
            // A COUNT_SCAN would not be incorrect here, just pointless: the clustered _id index and
            // the collection are the same tree, so counting it reads the whole collection anyway
            // and a CLUSTERED_IXSCAN/COLLSCAN is equivalent.
            for (const pipeline of kPipelines) {
                assert.eq(kDocCount, coll.aggregate(pipeline).toArray()[0].n);
                assert(
                    !hasCountScan(coll.explain().aggregate(pipeline)),
                    "did not expect a COUNT_SCAN for a clustered collection (no _id index entry)",
                );
            }
        });
    });

    describe("sharded collections", function () {
        const kDocCount = 100;
        const kMiddle = kDocCount / 2;
        let st;
        let shardedColl;
        let suspendFps;

        before(function () {
            st = new ShardingTest({shards: 2});
            shardedColl = st.getDB("test").sharded_count_scan;

            assert.commandWorked(st.s.getDB("admin").runCommand({enableSharding: "test"}));
            st.shardColl(
                shardedColl.getName(),
                {skey: 1},
                {skey: kMiddle},
                {skey: kMiddle + 1},
                "test",
                true,
            );

            assert.commandWorked(
                shardedColl.insertMany(
                    Array.from({length: kDocCount}, (_, i) => ({_id: i, skey: i})),
                ),
            );

            // Create permanent orphans deterministically: shardColl leaves the [kMiddle, MaxKey)
            // chunk on shard1, so move it back to shard0 with _waitForDelete: false. The donor's
            // copies of the moved range stay stranded on shard1 because the range deleter is
            // suspended for the lifetime of the test, so the pending range-deletion task is
            // never processed.
            suspendFps = st.rs1.nodes.map((n) => configureFailPoint(n, "suspendRangeDeletion"));

            assert.commandWorked(
                st.s.adminCommand({
                    moveChunk: shardedColl.getFullName(),
                    find: {skey: kDocCount - 1},
                    to: st.shard0.shardName,
                    _waitForDelete: false,
                }),
            );
        });

        after(function () {
            suspendFps.forEach((fp) => fp.off());
            shardedColl.drop();
            st.stop();
        });

        it("filters out orphans and gives up the COUNT_SCAN stage", function () {
            for (const pipeline of kPipelines) {
                // Verify orphans are not counted.
                assert.eq(kDocCount, shardedColl.aggregate(pipeline).toArray()[0].n);

                // Shard filtering requires reading the full documents which means the COUNT_SCAN
                // optimization cannot be used.
                const winningPlan = getWinningPlanFromExplain(
                    shardedColl.explain().aggregate(pipeline),
                );
                assert(
                    getPlanStages(winningPlan, "SHARDING_FILTER").length > 0,
                    "expected shard filtering in the winning plan",
                );
                assert(
                    getPlanStage(winningPlan, "COLLSCAN") !== null,
                    "expected a COLLSCAN in the winning plan when shard filtering is required",
                );
                assert(
                    getPlanStage(winningPlan, "COUNT_SCAN") === null,
                    "did not expect a COUNT_SCAN when shard filtering is required",
                );
                assert(
                    getPlanStage(winningPlan, "IXSCAN") === null,
                    "did not expect an IXSCAN when shard filtering is required",
                );
            }
        });
    });
});
