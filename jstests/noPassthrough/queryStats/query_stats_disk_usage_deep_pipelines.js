/**
 * Test that query stats are collected from data bearing nodes for aggregate queries involving
 * recursively nested pipelines (e.g., a $lookup into a $unionWith).
 *
 * @tags: [
 * featureFlagQueryStatsDataBearingNodes,
 * ]
 */

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {
    assertAggregatedMetricsSingleExec,
    clearPlanCacheAndQueryStatsStore,
    exhaustCursorAndGetQueryStats,
    getAggregateQueryStatsKey,
    runForEachDeployment,
} from "jstests/libs/query_stats_utils.js";

let collId = 0;
function getNewCollectionName() {
    return jsTestName() + collId++;
}

function getNewCollection(conn) {
    const name = getNewCollectionName();
    return conn.getDB("test")[name];
}

function makeUnshardedCollection(conn) {
    const coll = getNewCollection(conn);
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

function runLookupUnionWithPipelineTest(conn, collOuter) {
    const collMiddle = getNewCollection(conn);
    assert.commandWorked(collMiddle.insertMany([{v: 1, x: 1}, {v: 9, x: 2}]));
    assert.commandWorked(collMiddle.createIndex({v: 1}));

    const collInner = getNewCollection(conn);
    assert.commandWorked(
        collInner.insertMany([{v: 1, a: 1000}, {v: 2, a: 2000}, {v: 4000, a: 5000}]));

    const unionWith = {
        $unionWith: {
            coll: collInner.getName(),
            pipeline: [
                {$match: {v: {$lt: 3}}},
                {$sort: {a: 1}},
            ],
        }
    };
    const unionWithShape = {
        $unionWith: {
            coll: collInner.getName(),
            pipeline: [
                {$match: {v: {$lt: "?number"}}},
                {$sort: {a: 1}},
            ],
        }
    };

    const lookup = {$lookup: {
        from: collMiddle.getName(),
        as: "lookedUp",
        pipeline: [unionWith],
    }};
    const lookupShape = {$lookup: {
        from: collMiddle.getName(),
        as: "lookedUp",
        let: {},
        pipeline: [unionWithShape],
    }};

    const pipeline = [lookup];
    const shape = {
        pipeline: [
            lookupShape,
        ]
    };

    const otherNss = [
        {db: "test", coll: collInner.getName()},
        {db: "test", coll: collMiddle.getName()},
    ];
    const queryStatsKey =
        getAggregateQueryStatsKey(conn, collOuter.getName(), shape, {otherNss: otherNss});

    // The query takes documents matching v < 3 from collOuter (5 docs) does a lookup from those
    // to collMiddle (still 5 docs) and unions that result with documents from collInner matching
    // v < 3 (2 docs), giving a total of 7 docs.
    const expectedDocs = 7;
    for (let batchSize = 1; batchSize <= expectedDocs + 1; batchSize++) {
        clearPlanCacheAndQueryStatsStore(conn, collOuter);

        const cmd = {
            aggregate: collOuter.getName(),
            pipeline: pipeline,
            cursor: {batchSize: batchSize}
        };

        const queryStats =
            exhaustCursorAndGetQueryStats(conn, collOuter, cmd, queryStatsKey, expectedDocs);

        // The query is ultimately a collection scan over all three collections, 7 + 3 + 2 = 12
        // docsExamined.
        assertAggregatedMetricsSingleExec(queryStats, {
            keysExamined: 0,
            docsExamined: 12,
            hasSortStage: true,
            usedDisk: false,
            fromMultiPlanner: false,
            fromPlanCache: false
        });
    }
}

function runDeepBranchingPipelineTest(conn, coll1) {
    const coll2 = makeUnshardedCollection(conn);
    const coll3 = makeUnshardedCollection(conn);

    const otherNss = [
        {db: "test", coll: coll2.getName()},
        {db: "test", coll: coll3.getName()},
    ];

    // Each doc in the union below triggers an indexed lookup into coll3 here - 7 docs + 7 keys.
    const lookup = {$lookup: {
        from: coll3.getName(),
        as: "lookedUp",
        localField: "y",
        foreignField: "y",
    }};
    const lookupShape = {pipeline: [lookup]};

    // Collection scan over coll2 - 7 docs and 0 keys.
    const unionWithLookup = {
        $unionWith: {
            coll: coll2.getName(),
            pipeline: [lookup],
        }
    };
    const unionWithLookupShape = {
        $unionWith: {
            coll: coll2.getName(),
            pipeline: [lookupShape],
        }
    };

    // The match stage contributes 1 doc and 1 key.
    const pipelineUnionWithLookup = [
        {$match: {y: {$eq: 1}}},
        unionWithLookup,
    ];
    const pipelineUnionWithLookupShape = {
        pipeline: [
            {$match: {y: {$eq: "?number"}}},
            unionWithLookup,
        ]
    };

    {
        // 8 docs expected, one from coll1 unioned with 7 from coll2.
        const expectedDocs = 8;
        for (let batchSize = 1; batchSize <= expectedDocs + 1; batchSize++) {
            const batchSize = 1;
            clearPlanCacheAndQueryStatsStore(conn, coll1);

            const cmd = {
                aggregate: coll1.getName(),
                pipeline: pipelineUnionWithLookup,
                cursor: {batchSize: batchSize}
            };

            const queryStatsKey = getAggregateQueryStatsKey(
                conn, coll1.getName(), pipelineUnionWithLookupShape, {otherNss: otherNss});
            const queryStats =
                exhaustCursorAndGetQueryStats(conn, coll1, cmd, queryStatsKey, expectedDocs);

            assertAggregatedMetricsSingleExec(queryStats, {
                keysExamined: 8,
                docsExamined: 15,
                hasSortStage: false,
                usedDisk: false,
                fromMultiPlanner: false,
                fromPlanCache: false
            });
        }
    }

    const pipelineUnion = [
        {$match: {y: {$eq: 1}}},
        {$unionWith: {coll: coll1.getName(), pipeline: pipelineUnionWithLookup}},
        {$unionWith: {coll: coll1.getName(), pipeline: pipelineUnionWithLookup}},
    ];

    const pipelineUnionShape = {
        pipeline: [
            {$match: {y: {$eq: "?number"}}},
            {$unionWith: {coll: coll1.getName(), pipeline: pipelineUnionWithLookupShape.pipeline}},
            {$unionWith: {coll: coll1.getName(), pipeline: pipelineUnionWithLookupShape.pipeline}},
        ]
    };

    {
        const otherNssUnion = otherNss.concat({db: "test", coll: coll1.getName()});
        const expectedDocs = 17;
        for (let batchSize = 1; batchSize <= expectedDocs + 1; batchSize++) {
            clearPlanCacheAndQueryStatsStore(conn, coll1);

            const cmd = {
                aggregate: coll1.getName(),
                pipeline: pipelineUnion,
                cursor: {batchSize: batchSize},
            };

            const queryStatsKey = getAggregateQueryStatsKey(
                conn, coll1.getName(), pipelineUnionShape, {otherNss: otherNssUnion});
            const queryStats =
                exhaustCursorAndGetQueryStats(conn, coll1, cmd, queryStatsKey, expectedDocs);

            // Metrics for this query are generally the sums of those for the subpipelines, plus 1
            // key and 1 doc examined for the initial match stage.
            assertAggregatedMetricsSingleExec(queryStats, {
                keysExamined: 17,
                docsExamined: 31,
                hasSortStage: false,
                usedDisk: false,
                fromMultiPlanner: false,
                fromPlanCache: false
            });
        }
    }
}

function runTests(conn, coll) {
    runLookupUnionWithPipelineTest(conn, coll);
    runDeepBranchingPipelineTest(conn, coll);
}

runForEachDeployment((conn, test) => {
    const coll = makeUnshardedCollection(conn);
    runTests(conn, coll);

    if (conn.isMongos()) {
        const coll = makeShardedCollection(test);
        runTests(conn, coll);
    }
});
