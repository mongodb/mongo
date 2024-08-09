/**
 * Test that query stats are collected from data bearing nodes for aggregate queries involving
 * stages with nested piplines such as $lookup, $unionWith, and $facet.
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
import {checkSbeCompletelyDisabled} from "jstests/libs/sbe_util.js";

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

function runUnindexedLookupPipelineTest(conn, localColl) {
    const expectedDocs = 7;

    const foreignColl = getNewCollection(conn);
    foreignColl.insertMany([{v: 1, x: 1}, {v: 7, x: 2}, {v: 9000, x: 3}]);

    const lookup = {$lookup: {
        from: foreignColl.getName(),
        as: "lookedUp",
        localField: "v",
        foreignField: "v",
    }};
    const shape = {pipeline: [lookup]};

    const queryStatsKey = getAggregateQueryStatsKey(
        conn,
        localColl.getName(),
        shape,
        {otherNss: [{db: "test", coll: foreignColl.getName()}]},
    );

    for (let batchSize = 1; batchSize <= expectedDocs + 1; batchSize++) {
        clearPlanCacheAndQueryStatsStore(conn, localColl);

        const cmd = {
            aggregate: localColl.getName(),
            pipeline: [lookup],
            cursor: {batchSize: batchSize}
        };

        const queryStats =
            exhaustCursorAndGetQueryStats(conn, localColl, cmd, queryStatsKey, expectedDocs);

        // There are two cases for expectedDocs:
        // 1. The query can be a collection scan against the foreign collection for each doc in
        // the local collection. Each local doc is examined once and each foreign doc is examined
        // once for each local doc, giving N_{local} + N_{local} * N_{foreign} = 7 + 7*3 = 28.
        // 2. The query can have an EQ_LOOKUP stage with a HashJoin strategy. This examines
        // N_{local} + N_{foreign} = 10 documents.
        //
        // For the classic engine, it's always case 1. For SBE, a sharded collection gives case 2,
        // an unsharded collection gives case 1.
        const expectedDocsExamined = (() => {
            if (checkSbeCompletelyDisabled(conn.getDB("test"))) {
                // Classic engine.
                return 28;
            } else if (FixtureHelpers.isSharded(localColl)) {
                // SBE, sharded collection.
                return 28;
            } else {
                // SBE, not sharded.
                return 10;
            }
        })();
        assertAggregatedMetricsSingleExec(queryStats, {
            keysExamined: 0,
            docsExamined: expectedDocsExamined,
            hasSortStage: false,
            usedDisk: false,
            fromMultiPlanner: false,
            fromPlanCache: false
        });
    }
}

// The is similar to the unindexed lookup case, except we add an $_internalInhibitOptimization
// stage. This prevents the pipeline from using an optimized EQ_LOOKUP HashJoin to do the lookup,
// so it gives similar query stats in both sharded and unsharded cases.
function runUnindexedUnoptimizedLookupPipelineTest(conn, localColl) {
    const expectedDocs = 7;

    const foreignColl = getNewCollection(conn);
    foreignColl.insertMany([{v: 1, x: 1}, {v: 7, x: 2}, {v: 9000, x: 3}]);

    const lookup = {$lookup: {
        from: foreignColl.getName(),
        as: "lookedUp",
        localField: "v",
        foreignField: "v",
    }};
    const pipeline = [{$_internalInhibitOptimization: {}}, lookup];
    const shape = {pipeline: pipeline};

    const queryStatsKey = getAggregateQueryStatsKey(
        conn,
        localColl.getName(),
        shape,
        {otherNss: [{db: "test", coll: foreignColl.getName()}]},
    );

    for (let batchSize = 1; batchSize <= expectedDocs + 1; batchSize++) {
        clearPlanCacheAndQueryStatsStore(conn, localColl);

        const cmd = {
            aggregate: localColl.getName(),
            pipeline: pipeline,
            cursor: {batchSize: batchSize}
        };

        const queryStats =
            exhaustCursorAndGetQueryStats(conn, localColl, cmd, queryStatsKey, expectedDocs);

        // The lookup is a collection scan against the foreign collection for each document in the
        // local collection.
        // This gives N_{local} + N_{local} * N_{foreign} = 7 + 7*3 = 28 docsExamined.
        assertAggregatedMetricsSingleExec(queryStats, {
            keysExamined: 0,
            docsExamined: 28,
            hasSortStage: false,
            usedDisk: false,
            fromMultiPlanner: false,
            fromPlanCache: false
        });
    }
}

function runIndexedLookupPipelineTest(conn, localColl) {
    const expectedDocs = 7;

    const foreignColl = getNewCollection(conn);
    foreignColl.insertMany([{v: 1, x: 1}, {v: 7, x: 2}, {v: 9000, x: 3}]);
    assert.commandWorked(foreignColl.createIndex({v: 1}));

    const lookup = {$lookup: {
        from: foreignColl.getName(),
        as: "lookedUp",
        localField: "v",
        foreignField: "v",
    }};
    const shape = {pipeline: [lookup]};

    const queryStatsKey = getAggregateQueryStatsKey(
        conn,
        localColl.getName(),
        shape,
        {otherNss: [{db: "test", coll: foreignColl.getName()}]},
    );

    for (let batchSize = 1; batchSize <= expectedDocs + 1; batchSize++) {
        clearPlanCacheAndQueryStatsStore(conn, localColl);

        const cmd = {
            aggregate: localColl.getName(),
            pipeline: [lookup],
            cursor: {batchSize: batchSize}
        };

        const queryStats =
            exhaustCursorAndGetQueryStats(conn, localColl, cmd, queryStatsKey, expectedDocs);

        assertAggregatedMetricsSingleExec(queryStats, {
            keysExamined: 2,
            docsExamined: 9,
            hasSortStage: false,
            usedDisk: false,
            fromMultiPlanner: false,
            fromPlanCache: false
        });
    }
}

function runUnionWithPipelineTest(conn, coll1) {
    const coll2 = getNewCollection(conn);
    assert.commandWorked(coll2.insertMany([{a: 1, b: 1}, {a: 2, b: 2}, {a: 3, b: 3}]));

    const pipeline = [
        {$match: {v: {$gt: 1}}},
        {
            $unionWith: {
                coll: coll2.getName(),
                pipeline: [
                    {$match: {a: {$lt: 3}}},
                ],
            }
        },
    ];
    const shape = {
        pipeline: [
            {$match: {v: {$gt: "?number"}}},
            {
                $unionWith: {
                    coll: coll2.getName(),
                    pipeline: [
                        {$match: {a: {$lt: "?number"}}},
                    ],
                }
            },
        ]
    };

    const queryStatsKey = getAggregateQueryStatsKey(
        conn,
        coll1.getName(),
        shape,
        {otherNss: [{db: "test", coll: coll2.getName()}]},
    );

    // The query expects 6 docs from coll1 and 2 from coll2
    const expectedDocs = 8;
    for (let batchSize = 1; batchSize <= expectedDocs + 1; batchSize++) {
        clearPlanCacheAndQueryStatsStore(conn, coll1);

        const cmd = {
            aggregate: coll1.getName(),
            pipeline: pipeline,
            cursor: {batchSize: batchSize}
        };

        const queryStats =
            exhaustCursorAndGetQueryStats(conn, coll1, cmd, queryStatsKey, expectedDocs);

        // The query is a collection scan across both collections for 7 + 3 = 10 docs examined.
        assertAggregatedMetricsSingleExec(queryStats, {
            keysExamined: 0,
            docsExamined: 10,
            hasSortStage: false,
            usedDisk: false,
            fromMultiPlanner: false,
            fromPlanCache: false
        });
    }
}

function runIndexedUnionWithPipelineTest(conn, coll1) {
    const coll2 = getNewCollection(conn);
    assert.commandWorked(coll2.insertMany([{a: 1, b: 1}, {a: 2, b: 2}, {a: 3, b: 3}]));
    assert.commandWorked(coll2.createIndex({a: 1}));

    const pipeline = [
        {$match: {y: {$gt: -3}}},
        {
            $unionWith: {
                coll: coll2.getName(),
                pipeline: [
                    {$match: {a: {$lt: 3}}},
                    {$sort: {b: 1}},
                    {$limit: 2},
                ],
            }
        },
    ];
    const shape = {
        pipeline: [
            {$match: {y: {$gt: "?number"}}},
            {
                $unionWith: {
                    coll: coll2.getName(),
                    pipeline: [
                        {$match: {a: {$lt: "?number"}}},
                        {$sort: {b: 1}},
                        {$limit: "?number"},
                    ],
                }
            },
        ]
    };

    const queryStatsKey = getAggregateQueryStatsKey(
        conn,
        coll1.getName(),
        shape,
        {otherNss: [{db: "test", coll: coll2.getName()}]},
    );

    // The query expects 6 docs from coll1 and 2 from coll2
    const expectedDocs = 8;
    for (let batchSize = 1; batchSize <= expectedDocs + 1; batchSize++) {
        clearPlanCacheAndQueryStatsStore(conn, coll1);

        const cmd = {
            aggregate: coll1.getName(),
            pipeline: pipeline,
            cursor: {batchSize: batchSize}
        };

        const queryStats =
            exhaustCursorAndGetQueryStats(conn, coll1, cmd, queryStatsKey, expectedDocs);

        // The query can leverage the index on each collection to examine only the matching
        // documents.
        assertAggregatedMetricsSingleExec(queryStats, {
            keysExamined: 8,
            docsExamined: 8,
            hasSortStage: true,
            usedDisk: false,
            fromMultiPlanner: false,
            fromPlanCache: false
        });
    }
}

function runDocumentsLookupTest(conn, coll) {
    const expectedDocs = 2;

    // In the sharded case, this will join to documents in both shards.
    const documents = {
        $documents: [
            {y: -1},
            {y: 1},
        ]
    };
    const lookup = {$lookup: {
        from: coll.getName(),
        as: "lookedUp",
        localField: "y",
        foreignField: "y",
    }};
    const shape = {
        cmdNs: {db: "test", coll: "$cmd.aggregate"},
        pipeline: [
            {$queue: "?array<?object>"},
            {
                $project: {
                    _id: true,
                    "_tempDocumentsField": "?array<?object>",
                }
            },
            {
                $unwind: {
                    path: "$_tempDocumentsField",
                }
            },
            {
                $replaceRoot: {
                    newRoot: "$_tempDocumentsField",
                }
            }
        ].concat(lookup)
    };

    const queryStatsKey = getAggregateQueryStatsKey(
        conn, "$cmd.aggregate", shape, {otherNss: [{db: "test", coll: coll.getName()}]});

    for (let batchSize = 1; batchSize <= expectedDocs + 1; batchSize++) {
        clearPlanCacheAndQueryStatsStore(conn, coll);

        const cmd = {aggregate: 1, pipeline: [documents, lookup], cursor: {batchSize: batchSize}};

        const queryStats =
            exhaustCursorAndGetQueryStats(conn, coll, cmd, queryStatsKey, expectedDocs);

        assertAggregatedMetricsSingleExec(queryStats, {
            keysExamined: 2,
            docsExamined: 2,
            hasSortStage: false,
            usedDisk: false,
            fromMultiPlanner: false,
            fromPlanCache: false
        });
    }
}

function runFacetPipelineTest(conn, coll) {
    const pipeline = [
        {
            $facet: {
                "facetedByY": [
                    {$bucketAuto: {groupBy: "$y", buckets: 3}},
                ],
            }
        },
        {
            $facet: {
                "facetedByV": [
                    {$bucketAuto: {groupBy: "$v", buckets: 3}},
                ]
            }
        },
    ];
    const shape = {
        pipeline: [
            {
                $facet: {
                    "facetedByY": [
                        {
                            $bucketAuto: {
                                groupBy: "$y",
                                buckets: "?number",
                                output: {count: {$sum: "?number"}}
                            }
                        },
                    ],
                }
            },
            {
                $facet: {
                    "facetedByV": [
                        {
                            $bucketAuto: {
                                groupBy: "$v",
                                buckets: "?number",
                                output: {count: {$sum: "?number"}}
                            }
                        },
                    ],
                }
            }
        ]
    };

    const queryStatsKey = getAggregateQueryStatsKey(conn, coll.getName(), shape);

    // $facet returns a single document.
    const expectedDocs = 1;
    const batchSize = 1;
    clearPlanCacheAndQueryStatsStore(conn, coll);

    const cmd = {aggregate: coll.getName(), pipeline: pipeline, cursor: {batchSize: batchSize}};

    const queryStats = exhaustCursorAndGetQueryStats(conn, coll, cmd, queryStatsKey, expectedDocs);

    assertAggregatedMetricsSingleExec(queryStats, {
        keysExamined: 0,
        docsExamined: 7,
        hasSortStage: false,
        usedDisk: false,
        fromMultiPlanner: false,
        fromPlanCache: false
    });
}

function runTests(conn, coll) {
    runUnindexedLookupPipelineTest(conn, coll);
    runUnindexedUnoptimizedLookupPipelineTest(conn, coll);
    runIndexedLookupPipelineTest(conn, coll);
    runUnionWithPipelineTest(conn, coll);
    runIndexedUnionWithPipelineTest(conn, coll);
    runDocumentsLookupTest(conn, coll);
    runFacetPipelineTest(conn, coll);
}

runForEachDeployment((conn, test) => {
    const coll = makeUnshardedCollection(conn);
    runTests(conn, coll);

    if (conn.isMongos()) {
        const coll = makeShardedCollection(test);
        runTests(conn, coll);
    }
});
