/**
 * Test that query stats are collected from data bearing nodes for simple aggregate queries.
 * @tags: [requires_fcv_80]
 */

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {
    assertAggregatedMetricsSingleExec,
    clearPlanCacheAndQueryStatsStore,
    exhaustCursorAndGetQueryStats,
    getAggregateQueryStatsKey,
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

function runMatchSortPipelineTest(conn, coll) {
    const expectedDocs = 4;

    const shape = {
        pipeline:
            [{$match: {$and: [{v: {$gt: "?number"}}, {v: {$lt: "?number"}}]}}, {$sort: {v: 1}}]
    };

    const queryStatsKey = getAggregateQueryStatsKey(conn, coll.getName(), shape);

    for (let batchSize = 1; batchSize <= expectedDocs + 1; batchSize++) {
        clearPlanCacheAndQueryStatsStore(conn, coll);

        const cmd = {
            aggregate: coll.getName(),
            pipeline: [{$match: {v: {$gt: 0, $lt: 5}}}, {$sort: {v: 1}}],
            cursor: {batchSize: batchSize},
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

function runMatchPipelineTest(conn, coll) {
    const expectedDocs = 4;

    const shape = {pipeline: [{$match: {$and: [{v: {$gt: "?number"}}, {v: {$lt: "?number"}}]}}]};
    const queryStatsKey = getAggregateQueryStatsKey(conn, coll.getName(), shape);

    for (let batchSize = 1; batchSize <= expectedDocs + 1; batchSize++) {
        clearPlanCacheAndQueryStatsStore(conn, coll);

        const cmd = {
            aggregate: coll.getName(),
            pipeline: [{$match: {v: {$gt: 0, $lt: 5}}}],
            cursor: {batchSize: batchSize},
        };

        const queryStats =
            exhaustCursorAndGetQueryStats(conn, coll, cmd, queryStatsKey, expectedDocs);

        assertAggregatedMetricsSingleExec(queryStats, {
            keysExamined: 0,
            docsExamined: 7,
            hasSortStage: false,
            usedDisk: false,
            fromMultiPlanner: false,
            fromPlanCache: false
        });
    }
}

function runViewPipelineTest(conn, coll) {
    const viewName = jsTestName() + "_view";

    const testDB = conn.getDB("test");
    assert.commandWorked(testDB.createView(viewName, coll.getName(), [{$match: {v: {$gt: 1}}}]));
    const view = testDB[viewName];

    const expectedDocs = 3;
    const shape = {
        pipeline:
            [{$match: {$and: [{v: {$gt: "?number"}}, {v: {$lt: "?number"}}]}}, {$sort: {v: 1}}]
    };

    const queryStatsKey = getAggregateQueryStatsKey(conn, view.getName(), shape);

    for (let batchSize = 1; batchSize <= expectedDocs + 1; batchSize++) {
        clearPlanCacheAndQueryStatsStore(conn, view);

        const cmd = {
            aggregate: view.getName(),
            pipeline: [{$match: {v: {$gt: 0, $lt: 5}}}, {$sort: {v: 1}}],
            cursor: {batchSize: batchSize},
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

// With mongos, this pipeline should run entirely on the mongos, which is a case we want to cover.
// It can also run standalone.
function runCollStatsPipelineTest(conn, coll) {
    // One document is expected for each shard.
    const expectedDocs = FixtureHelpers.isSharded(coll) ? 2 : 1;

    const pipeline = [{$collStats: {}}, {$sort: {ns: 1}}];
    const shape = {pipeline: pipeline};

    const queryStatsKey = getAggregateQueryStatsKey(conn, coll.getName(), shape);

    for (let batchSize = 1; batchSize <= expectedDocs + 1; batchSize++) {
        clearPlanCacheAndQueryStatsStore(conn, coll);

        const cmd = {
            aggregate: coll.getName(),
            pipeline: pipeline,
            cursor: {batchSize: batchSize},
        };
        const queryStats =
            exhaustCursorAndGetQueryStats(conn, coll, cmd, queryStatsKey, expectedDocs);

        assertAggregatedMetricsSingleExec(queryStats, {
            keysExamined: 0,
            docsExamined: 0,
            hasSortStage: true,
            usedDisk: false,
            fromMultiPlanner: false,
            fromPlanCache: false
        });
    }
}

function runTests(conn, coll) {
    runMatchPipelineTest(conn, coll);
    runMatchSortPipelineTest(conn, coll);
    runViewPipelineTest(conn, coll);
    runCollStatsPipelineTest(conn, coll);
}

const options = {
    setParameter: {internalQueryStatsRateLimit: -1}
};

jsTestLog("Standalone: Testing query stats disk usage for aggregate queries");
{
    const conn = MongoRunner.runMongod(options);

    // Standalone tests
    const coll = makeUnshardedCollection(conn);
    runTests(conn, coll);

    MongoRunner.stopMongod(conn);
}

runForEachDeployment((conn, test) => {
    const coll = makeUnshardedCollection(conn);
    runTests(conn, coll);

    if (conn.isMongos()) {
        const coll = makeShardedCollection(test);
        runTests(conn, coll);
    }
});
