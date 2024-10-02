/**
 * Test that query stats are collected from data bearing nodes in specific sharding-specific
 * situations (e.g., a $unionWith targeting multiple sharded collections).
 *
 * @tags: [requires_fcv_80]
 */

import {
    assertAggregatedMetricsSingleExec,
    clearPlanCacheAndQueryStatsStore,
    exhaustCursorAndGetQueryStats,
    getAggregateQueryStatsKey,
} from "jstests/libs/query/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

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

function runLookupForeignShardedPipelineTest(st) {
    const expectedDocs = 7;
    const conn = st.s;

    const foreignColl = makeShardedCollection(st);
    const localColl = makeUnshardedCollection(st);

    const lookup = {$lookup: {
        from: foreignColl.getName(),
        as: "lookedUp",
        localField: "v",
        foreignField: "y",
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

        // The local and foreign collections contain the exact same set of documents.
        // We match local.v to foreign.y so that (1) the foreign column is indexed and (2) not all
        // documents match.
        const cmd = {
            aggregate: localColl.getName(),
            pipeline: [lookup],
            cursor: {batchSize: batchSize}
        };

        const queryStats =
            exhaustCursorAndGetQueryStats(conn, localColl, cmd, queryStatsKey, expectedDocs);

        // Due to the index, we look at all local documents plus only the matching foreign
        // documents.
        // We also look at index keys for the matching foreign documents.
        assertAggregatedMetricsSingleExec(queryStats, {
            keysExamined: 4,
            docsExamined: 11,
            hasSortStage: false,
            usedDisk: false,
            fromMultiPlanner: false,
            fromPlanCache: false
        });
    }
}

function runUnionWithShardedPipelineTest(st) {
    const conn = st.s;

    const coll1 = makeShardedCollection(st);
    const coll2 = makeShardedCollection(st);

    // We match y > -2 (5 documents across both shards, leveraging index) for coll1.
    // We match v > 2 (5 documents across both shards) for coll2. We also add a sort and a limit
    // to observe hasSortStage propagating.
    const pipeline = [
        {$match: {y: {$gt: -2}}},
        {
            $unionWith: {
                coll: coll2.getName(),
                pipeline: [
                    {$match: {v: {$gt: 2}}},
                    {$sort: {v: 1}},
                    {$limit: 10},
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
                        {$match: {v: {$gt: "?number"}}},
                        {$sort: {v: 1}},
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

    const expectedDocs = 10;
    for (let batchSize = 1; batchSize <= expectedDocs + 1; batchSize++) {
        clearPlanCacheAndQueryStatsStore(conn, coll1);

        const cmd = {
            aggregate: coll1.getName(),
            pipeline: pipeline,
            cursor: {batchSize: batchSize}
        };

        const queryStats =
            exhaustCursorAndGetQueryStats(conn, coll1, cmd, queryStatsKey, expectedDocs);

        // Index scan against coll1 for 5 docs examined and collection scan against coll2 for 7,
        // giving a total of 12 docs and 5 keys examined.
        assertAggregatedMetricsSingleExec(queryStats, {
            keysExamined: 5,
            docsExamined: 12,
            hasSortStage: true,
            usedDisk: false,
            fromMultiPlanner: false,
            fromPlanCache: false
        });
    }
}

const options = {
    setParameter: {internalQueryStatsRateLimit: -1}
};

const st = new ShardingTest(Object.assign({shards: 2, other: {mongosOptions: options}}));
const testDB = st.s.getDB("test");

runLookupForeignShardedPipelineTest(st);
runUnionWithShardedPipelineTest(st);

st.stop();
