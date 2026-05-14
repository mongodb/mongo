/**
 * Tests that mongos slow query log lines for aggregations targeting a view carry the correct
 * "collectionType" field.
 *
 * Today this is a bug-pinning test for SERVER-114178: on mongos, when an aggregate runs against a
 * view, the resolved-views map (see OpDebug::resolvedViews / view_catalog_helpers.cpp) is populated
 * on the shards but is empty by the time mongos emits its slow-query log line, so mongos records
 * collectionType: "normal" instead of "view". When SERVER-114178 is fixed, flip the assertion
 * marked with the TODO below from "normal" to "view".
 *
 * Companion to the queryStats coverage in
 * jstests/noPassthrough/query/queryStats/query_stats_collectionType.js — that test pins the
 * standalone/per-shard view collectionType; this one pins the cross-router emission.
 *
 * @tags: [
 *   requires_profiling,
 *   # The slow-query threshold is mutated via setProfilingLevel which doesn't survive stepdowns.
 *   does_not_support_stepdowns,
 *   # Slow-query log scraping interacts with the system profiler.
 *   does_not_support_causal_consistency,
 *   does_not_support_transactions,
 *   requires_fcv_83,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("mongos slow query log collectionType on view aggregations", function () {
    let st;
    let routerDB;
    let shard0DB;
    let shard1DB;

    const baseCollName = "base_coll";
    const viewName = "base_coll_view";

    before(function () {
        st = new ShardingTest({shards: 2, mongos: 1});
        assert(st.adminCommand({enableSharding: jsTestName(), primaryShard: st.shard0.shardName}));

        routerDB = st.s.getDB(jsTestName());
        shard0DB = st.shard0.getDB(jsTestName());
        shard1DB = st.shard1.getDB(jsTestName());

        // Seed the underlying sharded collection and split it across both shards so the router
        // actually dispatches the aggregate (rather than collapsing to a single-shard pass-through,
        // which can short-circuit the resolved-views propagation we are pinning).
        const baseColl = routerDB[baseCollName];
        baseColl.drop();
        assert.commandWorked(routerDB.createCollection(baseCollName));
        baseColl.createIndex({y: 1});
        assert.commandWorked(
            routerDB.adminCommand({shardCollection: baseColl.getFullName(), key: {y: 1}}),
        );
        baseColl.insertMany([
            {_id: 0, x: 4, y: 1},
            {_id: 1, x: 4, y: 2},
            {_id: 2, x: 5, y: 2},
            {_id: 3, x: 6, y: 3},
            {_id: 4, x: 7, y: 3},
        ]);
        assert.commandWorked(routerDB.adminCommand({split: baseColl.getFullName(), middle: {y: 2}}));
        assert.commandWorked(
            routerDB.adminCommand({
                moveChunk: baseColl.getFullName(),
                find: {y: 3},
                to: st.shard1.shardName,
            }),
        );

        // Identity view on top of the base collection — view resolution is the code path under
        // test; we deliberately keep the pipeline simple so the slow log is easy to reason about.
        routerDB[viewName].drop();
        assert.commandWorked(routerDB.createView(viewName, baseCollName, [{$addFields: {z: 1}}]));

        // Force every command to be logged as slow on both the router and the shards.
        routerDB.setProfilingLevel(0, -1);
        shard0DB.setProfilingLevel(0, -1);
        shard1DB.setProfilingLevel(0, -1);
    });

    after(function () {
        st.stop();
    });

    function getSlowQueryLogLines({queryComment, testDB}) {
        return assert
            .commandWorked(testDB.adminCommand({getLog: "global"}))
            .log.map((entry) => JSON.parse(entry))
            .filter((entry) => {
                if (entry.msg !== "Slow query" || !entry.attr || !entry.attr.command) {
                    return false;
                }
                // Filter out the getMore continuation lines — we want the originating aggregate.
                if (entry.attr.command.getMore) {
                    return false;
                }
                return entry.attr.command.comment === queryComment;
            });
    }

    function uniqueComment(label) {
        return `!!SERVER-114178 ${label} ${UUID().toString()}`;
    }

    it("records collectionType for aggregate against the base collection on mongos", function () {
        const comment = uniqueComment("base-coll-agg");
        assert.commandWorked(
            routerDB.runCommand({
                aggregate: baseCollName,
                pipeline: [{$match: {x: 4}}],
                cursor: {batchSize: 0},
                comment: comment,
            }),
        );

        const routerLogs = getSlowQueryLogLines({queryComment: comment, testDB: routerDB});
        assert.gt(routerLogs.length, 0, "expected a mongos slow-query log line for base aggregate");
        routerLogs.forEach((logLine) => {
            assert.eq(
                logLine.attr.collectionType,
                "normal",
                "base collection should be reported as 'normal' on mongos: " + tojson(logLine),
            );
        });
    });

    it("records collectionType for aggregate against a view on mongos (SERVER-114178)", function () {
        const comment = uniqueComment("view-agg");
        assert.commandWorked(
            routerDB.runCommand({
                aggregate: viewName,
                pipeline: [{$match: {x: 4}}],
                cursor: {batchSize: 0},
                comment: comment,
            }),
        );

        const routerLogs = getSlowQueryLogLines({queryComment: comment, testDB: routerDB});
        assert.gt(routerLogs.length, 0, "expected a mongos slow-query log line for view aggregate");

        routerLogs.forEach((logLine) => {
            assert(
                logLine.attr.hasOwnProperty("collectionType"),
                "mongos slow query log missing collectionType: " + tojson(logLine),
            );

            // The aggregation targeted a view, so the namespace logged on mongos should be the
            // view's namespace (not the resolved underlying collection).
            assert.eq(
                logLine.attr.ns,
                `${jsTestName()}.${viewName}`,
                "expected mongos slow log ns to be the view namespace: " + tojson(logLine),
            );

            // TODO SERVER-114178: mongos currently reports "normal" because the resolvedViews map
            // on the router's OpDebug is empty at log-emission time. Once the fix propagates the
            // collection type back from the shards (or saves it off in AdditiveMetrics during the
            // query lifecycle), flip this expected value to "view" and remove this TODO.
            const expectedRouterCollectionType = "normal";
            assert.eq(
                logLine.attr.collectionType,
                expectedRouterCollectionType,
                "mongos slow log collectionType for view aggregate did not match the pinned " +
                    "buggy value — if SERVER-114178 has been fixed, flip the expected value to " +
                    "'view' and update the TODO. Log line: " + tojson(logLine),
            );
        });

        // Shards see the resolved view and should already report the correct collection type for
        // any per-shard slow-log entries. We don't strictly require a slow log on every shard
        // (the router may target only one shard depending on planner choices), but if one is
        // present its collectionType should not be "normal".
        for (const shardDB of [shard0DB, shard1DB]) {
            const shardLogs = getSlowQueryLogLines({queryComment: comment, testDB: shardDB});
            shardLogs.forEach((logLine) => {
                assert(
                    logLine.attr.hasOwnProperty("collectionType"),
                    "shard slow query log missing collectionType: " + tojson(logLine),
                );
                assert.neq(
                    logLine.attr.collectionType,
                    "normal",
                    "shard saw resolved view but reported collectionType 'normal': " +
                        tojson(logLine),
                );
            });
        }
    });
});
