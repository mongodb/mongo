/**
 * Test that query stats are collected from data bearing nodes for find queries.
 * @tags: [
 * featureFlagQueryStatsDataBearingNodes,
 * ]
 */

import {
    assertAggregatedMetricsSingleExec,
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

function runUnindexedFindTest(conn, coll) {
    const expectedDocs = 4;
    const shape = {filter: {$and: [{v: {$gt: "?number"}}, {v: {$lt: "?number"}}]}, sort: {v: 1}};

    const queryStatsKey = getFindQueryStatsKey(conn, coll.getName(), shape);

    for (let batchSize = 1; batchSize <= expectedDocs + 1; batchSize++) {
        clearPlanCacheAndQueryStatsStore(conn, coll);

        const cmd = {
            find: coll.getName(),
            filter: {v: {$gt: 0, $lt: 5}},
            sort: {v: 1},
            batchSize: batchSize
        };

        const queryStats =
            exhaustCursorAndGetQueryStats(conn, coll, cmd, queryStatsKey, expectedDocs);

        assertAggregatedMetricsSingleExec(queryStats, {
            keysExamined: 0,
            docsExamined: 7,
            hasSortStage: true,
            usedDisk: false,
            fromMultiPlanner: false,
            fromPlanCache: false
        });
    }
}

// The "indexed" aspect of this test is relevant as it allows shard targeting in the sharded
// case. It also allows us to test the keysExamined metric.
function runIndexedFindTest(conn, coll) {
    const expectedDocs = 4;
    const shape = {filter: {y: {$gt: "?number"}}, sort: {v: 1}};

    const queryStatsKey = getFindQueryStatsKey(conn, coll.getName(), shape);

    // Results should be the same independent of batch size. We need to reach a batch size of
    // docsReturned + 1 for the initial find command to return an exhausted cursor.
    for (let batchSize = 1; batchSize <= expectedDocs + 1; batchSize++) {
        clearPlanCacheAndQueryStatsStore(conn, coll);

        // In the sharded case, this will target only one shard.
        const cmd =
            {find: coll.getName(), filter: {y: {$gt: 0}}, sort: {v: 1}, batchSize: batchSize};
        const queryStats =
            exhaustCursorAndGetQueryStats(conn, coll, cmd, queryStatsKey, expectedDocs);

        assertAggregatedMetricsSingleExec(queryStats, {
            keysExamined: 4,
            docsExamined: 4,
            hasSortStage: true,
            usedDisk: false,
            fromMultiPlanner: false,
            fromPlanCache: false
        });
    }
}

function runFindAgainstViewTest(conn, coll) {
    // Note: the "find against a view" may be rewritten as an aggregate internally.
    const viewName = jsTestName() + "_view";

    const testDB = conn.getDB("test");
    assert.commandWorked(testDB.createView(viewName, coll.getName(), [{$match: {v: {$gt: 1}}}]));
    const view = testDB[viewName];

    const expectedDocs = 3;
    const shape = {filter: {$and: [{v: {$gt: "?number"}}, {v: {$lt: "?number"}}]}, sort: {v: 1}};

    const queryStatsKey = getFindQueryStatsKey(conn, view.getName(), shape);

    for (let batchSize = 1; batchSize <= expectedDocs + 1; batchSize++) {
        clearPlanCacheAndQueryStatsStore(conn, coll);

        const cmd = {
            find: view.getName(),
            filter: {v: {$gt: 0, $lt: 5}},
            sort: {v: 1},
            batchSize: batchSize
        };

        const queryStats =
            exhaustCursorAndGetQueryStats(conn, view, cmd, queryStatsKey, expectedDocs);

        // The view only contains 6 documents, but the query still ends up being a collection
        // scan of all docs in the collection.
        assertAggregatedMetricsSingleExec(queryStats, {
            keysExamined: 0,
            docsExamined: 7,
            hasSortStage: true,
            usedDisk: false,
            fromMultiPlanner: false,
            fromPlanCache: false
        });
    }
}

function runTests(conn, coll) {
    runUnindexedFindTest(conn, coll);
    runIndexedFindTest(conn, coll);
    runFindAgainstViewTest(conn, coll);
}

runForEachDeployment((conn, test) => {
    const coll = makeUnshardedCollection(conn);
    runTests(conn, coll);

    if (conn.isMongos()) {
        const coll = makeShardedCollection(test);
        runTests(conn, coll);
    }
});
