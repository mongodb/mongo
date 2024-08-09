/**
 * Test that query stats regarding the plan cache are collected from data bearing nodes.
 * @tags: [
 * featureFlagQueryStatsDataBearingNodes,
 * ]
 */

import {
    assertAggregatedBoolean,
    clearPlanCacheAndQueryStatsStore,
    exhaustCursorAndGetQueryStats,
    getFindQueryStatsKey,
    runForEachDeployment,
} from "jstests/libs/query_stats_utils.js";

function makeUnshardedCollection(conn) {
    const coll = conn.getDB("test")[jsTestName()];
    coll.drop();
    assert.commandWorked(coll.insert([
        {v: 1, y: -3},
        {v: 2, y: -2},
        {v: 3, y: -1},
        {v: 4, y: 1},
        {v: 5, y: 2},
        {v: 6, y: 3},
        {v: 7, y: 4}
    ]));
    assert.commandWorked(coll.createIndex({y: 1}));
    assert.commandWorked(coll.createIndex({v: 1}));
    return coll;
}

function makeShardedCollection(st) {
    const conn = st.s;
    const coll = makeUnshardedCollection(conn);
    st.shardColl(coll,
                 /* key */ {y: 1},
                 /* split at */ {y: 0},
                 /* move chunk containing */ {y: 1},
                 /* db */ coll.getDB().getName(),
                 /* waitForDelete */ true);
    return coll;
}

// It's important that the collection has an index on both y and v, as otherwise there will only
// be one plan and the plan cache won't be used.
function runCachedPlanTest(conn, coll) {
    const expectedDocs = 5;
    const shape = {filter: {$and: [{v: {$gt: "?number"}}, {y: {$gt: "?number"}}]}};

    const queryStatsKey = getFindQueryStatsKey(conn, coll.getName(), shape);

    for (let batchSize = 1; batchSize <= expectedDocs + 1; batchSize++) {
        clearPlanCacheAndQueryStatsStore(conn, coll);

        const cmd = {
            find: coll.getName(),
            filter: {v: {$gt: -2}, y: {$gt: -2}},
            batchSize: batchSize
        };

        // No entries in the plan cache - this query will definitely not use the plan cache.
        const queryStatsColdCache =
            exhaustCursorAndGetQueryStats(conn, coll, cmd, queryStatsKey, expectedDocs);
        assertAggregatedBoolean(
            queryStatsColdCache, "fromPlanCache", {trueCount: 0, falseCount: 1});

        // Inactive entry in the plan cache - we still won't use the plan cache here.
        const queryStatsInactiveCache =
            exhaustCursorAndGetQueryStats(conn, coll, cmd, queryStatsKey, expectedDocs);
        assertAggregatedBoolean(
            queryStatsInactiveCache, "fromPlanCache", {trueCount: 0, falseCount: 2});

        // Active entry in the plan cache - we will use the plan cache.
        const queryStatsActiveCache =
            exhaustCursorAndGetQueryStats(conn, coll, cmd, queryStatsKey, expectedDocs);
        assertAggregatedBoolean(
            queryStatsActiveCache, "fromPlanCache", {trueCount: 1, falseCount: 2});
    }
}

runForEachDeployment((conn, test) => {
    const coll = makeUnshardedCollection(conn);
    runCachedPlanTest(conn, coll);

    if (conn.isMongos()) {
        const coll = makeShardedCollection(test);
        runCachedPlanTest(conn, coll);
    }
});
