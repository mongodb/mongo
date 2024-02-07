/**
 * Test that mongos is collecting query stats metrics for find queries.
 * @tags: [
 * featureFlagQueryStatsDataBearingNodes,
 * ]
 */

import {
    assertAggregatedBoolean,
    assertAggregatedMetric,
    assertExpectedResults,
    getLatestQueryStatsEntry,
    getQueryStats
} from "jstests/libs/query_stats_utils.js";

function resetCollection(coll) {
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
}

function setupShardedCollection(st, db, coll) {
    assert.commandWorked(
        db.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));

    resetCollection(coll);

    assert.commandWorked(coll.createIndex({y: 1}));
    st.shardColl(coll,
                 /* key */ {y: 1},
                 /* split at */ {y: 0},
                 /* move chunk containing */ {y: 1},
                 /* db */ coll.getDB().getName(),
                 /* waitForDelete */ true);
}

function runFindOnlyTest(conn, coll) {
    const db = conn.getDB("test");

    // This is most of the query stats key. There are mongod- and mongos-specific details that
    // are added conditionally afterwards.
    const queryStatsKey = {
        queryShape: {
            cmdNs: {db: "test", coll: coll.getName()},
            command: "find",
            filter: {$and: [{v: {$gt: "?number"}}, {v: {$lt: "?number"}}]},
            sort: {v: 1}
        },
        client: {application: {name: "MongoDB Shell"}}
    };

    if (conn.isMongos()) {
        queryStatsKey.readConcern = {level: "local", provenance: "implicitDefault"};
    } else {
        queryStatsKey.collectionType = "collection";
    }

    assert.commandWorked(
        db.runCommand({find: coll.getName(), filter: {v: {$gt: 0, $lt: 5}}, sort: {v: 1}}));

    const queryStats = getLatestQueryStatsEntry(conn, {collName: coll.getName()});
    print("Query Stats: " + tojson(queryStats));

    assertExpectedResults(queryStats,
                          queryStatsKey,
                          /* expectedExecCount */ 1,
                          /* expectedDocsReturnedSum */ 4,
                          /* expectedDocsReturnedMax */ 4,
                          /* expectedDocsReturnedMin */ 4,
                          /* expectedDocsReturnedSumOfSq */ 16,
                          /* getMores */ false);

    assertAggregatedMetric(queryStats, "keysExamined", {sum: 0, min: 0, max: 0, sumOfSq: 0});
    assertAggregatedMetric(queryStats, "docsExamined", {sum: 7, min: 7, max: 7, sumOfSq: 49});

    assertAggregatedBoolean(queryStats, "hasSortStage", {trueCount: 1, falseCount: 0});
    assertAggregatedBoolean(queryStats, "usedDisk", {trueCount: 0, falseCount: 1});
    assertAggregatedBoolean(queryStats, "fromMultiPlanner", {trueCount: 0, falseCount: 1});
    assertAggregatedBoolean(queryStats, "fromPlanCache", {trueCount: 0, falseCount: 1});
}

function runFindGetMoreTest(conn, coll) {
    const db = conn.getDB("test");

    const queryStatsKey = {
        queryShape: {
            cmdNs: {db: "test", coll: coll.getName()},
            command: "find",
            filter: {$and: [{v: {$gt: "?number"}}, {v: {$lt: "?number"}}]},
            sort: {v: 1}
        },
        batchSize: "?number",
        client: {application: {name: "MongoDB Shell"}}
    };

    if (conn.isMongos()) {
        queryStatsKey.readConcern = {level: "local", provenance: "implicitDefault"};
    } else {
        queryStatsKey.collectionType = "collection";
    }

    const cursor = coll.find({v: {$gt: 0, $lt: 5}}).sort({v: 1}).batchSize(1);

    // Since the cursor hasn't been exhausted yet, ensure no query stats results have been written
    // yet.
    let queryStats = getQueryStats(db, {collName: coll.getName()});
    assert.eq(0, queryStats.length, queryStats);

    // Run a getMore that doesn't exhaust the cursor, and check again for no query stats.
    assert.commandWorked(
        db.runCommand({getMore: cursor.getId(), collection: coll.getName(), batchSize: 2}));
    queryStats = getQueryStats(db, {collName: coll.getName()});
    assert.eq(0, queryStats.length, queryStats);

    // Run a getMore to exhaust the cursor, then ensure query stats results have been written
    // accurately. batchSize must be 5 so the cursor recognizes exhaustion.
    assert.commandWorked(
        db.runCommand({getMore: cursor.getId(), collection: coll.getName(), batchSize: 5}));

    queryStats = getLatestQueryStatsEntry(conn, {collName: coll.getName()});
    print("Query Stats: " + tojson(queryStats));

    assertExpectedResults(queryStats,
                          queryStatsKey,
                          /* expectedExecCount */ 1,
                          /* expectedDocsReturnedSum */ 4,
                          /* expectedDocsReturnedMax */ 4,
                          /* expectedDocsReturnedMin */ 4,
                          /* expectedDocsReturnedSumOfSq */ 16,
                          /* getMores */ true);

    assertAggregatedMetric(queryStats, "keysExamined", {sum: 0, min: 0, max: 0, sumOfSq: 0});
    assertAggregatedMetric(queryStats, "docsExamined", {sum: 7, min: 7, max: 7, sumOfSq: 49});

    // true, false
    assertAggregatedBoolean(queryStats, "hasSortStage", {trueCount: 1, falseCount: 0});
    assertAggregatedBoolean(queryStats, "usedDisk", {trueCount: 0, falseCount: 1});
    assertAggregatedBoolean(queryStats, "fromMultiPlanner", {trueCount: 0, falseCount: 1});
    assertAggregatedBoolean(queryStats, "fromPlanCache", {trueCount: 0, falseCount: 1});
}

function runTest(conn, resetCollectionFn) {
    const db = conn.getDB("test");

    const collFindOnly = db[jsTestName() + "_find_only_test"];
    resetCollectionFn(collFindOnly);
    runFindOnlyTest(conn, collFindOnly);

    if (!conn.isMongos()) {
        // TODO SERVER-83646: Enable the getMore part of this test for mongos.
        const collFindGetMore = db[jsTestName() + "_find_get_more"];
        resetCollectionFn(collFindGetMore);
        runFindGetMoreTest(conn, collFindGetMore);
    }
}

const options = {
    setParameter: {internalQueryStatsRateLimit: -1}
};

jsTestLog("Standalone: Testing query stats disk usage for mongod");
{
    const conn = MongoRunner.runMongod(options);

    runTest(conn, resetCollection);

    MongoRunner.stopMongod(conn);
}

jsTestLog("Sharded cluster: Testing query stats disk usage for mongos");
{
    let st = new ShardingTest(Object.assign({shards: 2, other: {mongosOptions: options}}));

    let db = st.s.getDB("test");

    runTest(st.s, (coll) => {
        setupShardedCollection(st, db, coll);
    });

    st.stop();
}
