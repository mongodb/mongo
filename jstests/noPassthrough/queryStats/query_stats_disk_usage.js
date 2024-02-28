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

let collId = 0;
function getNewCollection(conn) {
    // This function lets us run each test case against a different collection. This is needed
    // so that query stats don't leak between test cases, as dropping a collection and recreating
    // it doesn't clear associated entries in the query stats store.
    const db = conn.getDB("test");
    const name = jsTestName() + collId++;
    return db[name];
}

function setupUnshardedCollection(coll) {
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
}

function setupShardedCollection(st, db, coll) {
    setupUnshardedCollection(coll);
    st.shardColl(coll,
                 /* key */ {y: 1},
                 /* split at */ {y: 0},
                 /* move chunk containing */ {y: 1},
                 /* db */ coll.getDB().getName(),
                 /* waitForDelete */ true);
}

function getQueryStatsKey(conn, coll, queryShapeExtra = {}, extra = {}) {
    // This is most of the query stats key. There are mongod- and mongos-specific details that
    // are added conditionally afterwards.
    const baseQueryShape = {
        cmdNs: {db: "test", coll: coll.getName()},
        command: "find",
        filter: {$and: [{v: {$gt: "?number"}}, {v: {$lt: "?number"}}]},
        sort: {v: 1}
    };
    const baseStatsKey = {
        queryShape: Object.assign(baseQueryShape, queryShapeExtra),
        client: {application: {name: "MongoDB Shell"}}
    };
    const queryStatsKey = Object.assign(baseStatsKey, extra);

    if (conn.isMongos()) {
        queryStatsKey.readConcern = {level: "local", provenance: "implicitDefault"};
    } else {
        queryStatsKey.collectionType = "collection";
    }

    return queryStatsKey;
}

function runFindOnlyTest(conn, coll) {
    const db = conn.getDB("test");

    const queryStatsKey = getQueryStatsKey(conn, coll);

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

    const queryStatsKey = getQueryStatsKey(conn, coll, {}, {batchSize: "?number"});

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

    assertAggregatedBoolean(queryStats, "hasSortStage", {trueCount: 1, falseCount: 0});
    assertAggregatedBoolean(queryStats, "usedDisk", {trueCount: 0, falseCount: 1});
    assertAggregatedBoolean(queryStats, "fromMultiPlanner", {trueCount: 0, falseCount: 1});
    assertAggregatedBoolean(queryStats, "fromPlanCache", {trueCount: 0, falseCount: 1});
}

// The "indexed" aspect of this test is relevant as it allows shard targeting in the sharded
// case. It also allows us to test the keysExamined metric.
function runIndexedFindOnlyTest(conn, coll) {
    const db = conn.getDB("test");

    const queryStatsKey = getQueryStatsKey(conn, coll, {filter: {y: {$gt: "?number"}}});

    assert.commandWorked(
        db.runCommand({find: coll.getName(), filter: {y: {$gt: 0}}, sort: {v: 1}}));

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

    assertAggregatedMetric(queryStats, "keysExamined", {sum: 4, min: 4, max: 4, sumOfSq: 16});
    assertAggregatedMetric(queryStats, "docsExamined", {sum: 4, min: 4, max: 4, sumOfSq: 16});

    assertAggregatedBoolean(queryStats, "hasSortStage", {trueCount: 1, falseCount: 0});
    assertAggregatedBoolean(queryStats, "usedDisk", {trueCount: 0, falseCount: 1});
    assertAggregatedBoolean(queryStats, "fromMultiPlanner", {trueCount: 0, falseCount: 1});
    assertAggregatedBoolean(queryStats, "fromPlanCache", {trueCount: 0, falseCount: 1});
}

// The "indexed" aspect of this test is relevant as it allows shard targeting in the sharded
// case. It also allows us to test the keysExamined metric.
function runIndexedFindGetMoreTest(conn, coll) {
    const db = conn.getDB("test");

    const queryStatsKey =
        getQueryStatsKey(conn, coll, {filter: {y: {$gt: "?number"}}}, {batchSize: "?number"});

    const cursor = coll.find({y: {$gt: 0}}).sort({v: 1}).batchSize(1);

    // Since the cursor hasn't been exhausted yet, ensure no query stats results have been written
    // yet.
    let queryStats = getQueryStats(db, {collName: coll.getName()});
    assert.eq(0, queryStats.length, queryStats);

    // Run a getMore to exhaust the cursor, then ensure query stats results have been written
    // accurately. batchSize must be 4 so the cursor recognizes exhaustion.
    assert.commandWorked(
        db.runCommand({getMore: cursor.getId(), collection: coll.getName(), batchSize: 4}));

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

    assertAggregatedMetric(queryStats, "keysExamined", {sum: 4, min: 4, max: 4, sumOfSq: 16});
    assertAggregatedMetric(queryStats, "docsExamined", {sum: 4, min: 4, max: 4, sumOfSq: 16});

    assertAggregatedBoolean(queryStats, "hasSortStage", {trueCount: 1, falseCount: 0});
    assertAggregatedBoolean(queryStats, "usedDisk", {trueCount: 0, falseCount: 1});
    assertAggregatedBoolean(queryStats, "fromMultiPlanner", {trueCount: 0, falseCount: 1});
    assertAggregatedBoolean(queryStats, "fromPlanCache", {trueCount: 0, falseCount: 1});
}

function runTest(conn, resetCollectionFn) {
    const db = conn.getDB("test");

    const collFindOnly = getNewCollection(conn);
    resetCollectionFn(collFindOnly);
    runFindOnlyTest(conn, collFindOnly);

    const collFindGetMore = getNewCollection(conn);
    resetCollectionFn(collFindGetMore);
    runFindGetMoreTest(conn, collFindGetMore);

    const collIndexedFindOnly = getNewCollection(conn);
    resetCollectionFn(collIndexedFindOnly);
    runIndexedFindOnlyTest(conn, collIndexedFindOnly);

    const collIndexedFindGetMore = getNewCollection(conn);
    resetCollectionFn(collIndexedFindGetMore);
    runIndexedFindGetMoreTest(conn, collIndexedFindGetMore);
}

const options = {
    setParameter: {internalQueryStatsRateLimit: -1}
};

jsTestLog("Standalone: Testing query stats disk usage for mongod");
{
    const conn = MongoRunner.runMongod(options);

    runTest(conn, setupUnshardedCollection);

    MongoRunner.stopMongod(conn);
}

jsTestLog("Sharded cluster: Testing query stats disk usage for mongos");
{
    let st = new ShardingTest(Object.assign({shards: 2, other: {mongosOptions: options}}));

    let db = st.s.getDB("test");
    // Enable sharding separate from per-test setup to avoid calling enableSharding repeatedly.
    assert.commandWorked(
        db.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));

    runTest(st.s, setupUnshardedCollection);
    runTest(st.s, (coll) => {
        setupShardedCollection(st, db, coll);
    });

    st.stop();
}
