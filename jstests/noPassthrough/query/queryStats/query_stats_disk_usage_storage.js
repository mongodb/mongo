/**
 * Test that query stats collected from data-bearing nodes include storage metrics.
 * @tags: [
 *      # This test doesn't work with the in-memory storage engine
 *      requires_persistence,
 *      requires_fcv_80
 * ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {
    assertAggregatedMetricsSingleExec,
    assertExpectedResults,
    clearPlanCacheAndQueryStatsStore,
    exhaustCursorAndGetQueryStats,
    getCountQueryStatsKey,
    getDistinctQueryStatsKey,
    getFindQueryStatsKey,
    getQueryStatsCountCmd,
    getQueryStatsDistinctCmd,
    getQueryStatsServerParameters
} from "jstests/libs/query/query_stats_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

/**
 * INITIALIZATION
 */

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

/**
 * CALLBACK FUNCTIONS
 */

function runStorageStatsTestFind(conn, coll) {
    const cmd = {find: coll.getName(), filter: {}, batchSize: 100};
    const shape = {filter: {}};
    const expectedDocs = 7;

    const queryStatsKey = getFindQueryStatsKey(conn, coll.getName(), shape);
    clearPlanCacheAndQueryStatsStore(conn, coll);

    const queryStats = exhaustCursorAndGetQueryStats(conn, coll, cmd, queryStatsKey, expectedDocs);

    // The assertions in exhaustCursorAndGetQueryStats will check that the metrics are
    // sensible (e.g., max >= min), so we'll just assert that the minimum > 0.
    assert.gt(queryStats.metrics.bytesRead.min, 0, queryStats);
    // The read time can be zero even when the number of bytes read is non-zero, so we can assert
    // a non-negative value here.
    assert.gte(queryStats.metrics.readTimeMicros.min, 0);
}

function runStorageStatsTestDistinct(conn, coll) {
    if (!FeatureFlagUtil.isEnabled(conn, "QueryStatsCountDistinct"))
        return;

    const testDB = conn.getDB("test");
    const cmd = {distinct: coll.getName(), key: "y"};
    const shape = {key: "y"};
    const expectedDocs = 7;

    const queryStatsKey = getDistinctQueryStatsKey(conn, coll.getName(), shape);
    clearPlanCacheAndQueryStatsStore(conn, coll);

    assert.commandWorked(testDB.runCommand(cmd));
    const queryStats = getQueryStatsDistinctCmd(conn);
    assert.eq(queryStats.length, 1, queryStats);

    assertAggregatedMetricsSingleExec(queryStats[0], {
        keysExamined: 7,
        docsExamined: 0,
        hasSortStage: false,
        usedDisk: false,
        fromMultiPlanner: false,
        fromPlanCache: false
    });
    assertExpectedResults(queryStats[0],
                          queryStatsKey,
                          /* expectedExecCount */ 1,
                          /* expectedDocsReturnedSum */ expectedDocs,
                          /* expectedDocsReturnedMax */ expectedDocs,
                          /* expectedDocsReturnedMin */ expectedDocs,
                          /* expectedDocsReturnedSumOfSq */ expectedDocs ** 2,
                          false);

    assert.gt(queryStats[0].metrics.bytesRead.min, 0);
    assert.gte(queryStats[0].metrics.readTimeMicros.min, 0);
}

/**
 * Check the disk storage metrics of a count command.
 *
 * @param {*} conn - The connection to the mongo instance.
 * @param {*} coll - The collection to run the count command on.
 */
function runStorageStatsTestCount(conn, coll) {
    if (!FeatureFlagUtil.isEnabled(conn, "QueryStatsCountDistinct"))
        return;

    const testDB = conn.getDB("test");
    // Query provided so that the count command doesn't just use the collection metadata and skip
    // any keys due to the index.
    const cmd = {count: coll.getName(), query: {$or: [{v: 4}, {v: {$lte: 2}}]}};
    const shape = {query: {$or: [{v: {$eq: "?number"}}, {v: {$lte: "?number"}}]}};

    const queryStatsKey = getCountQueryStatsKey(conn, coll.getName(), shape);
    clearPlanCacheAndQueryStatsStore(conn, coll);

    assert.commandWorked(testDB.runCommand(cmd));
    const queryStats = getQueryStatsCountCmd(conn);
    assert.eq(queryStats.length, 1, queryStats);

    assertAggregatedMetricsSingleExec(queryStats[0], {
        keysExamined: 0,
        docsExamined: 7,
        hasSortStage: false,
        usedDisk: false,
        fromMultiPlanner: false,
        fromPlanCache: false
    });
    assertExpectedResults(queryStats[0],
                          queryStatsKey,
                          /* expectedExecCount */ 1,
                          /* expectedDocsReturnedSum */ 1,
                          /* expectedDocsReturnedMax */ 1,
                          /* expectedDocsReturnedMin */ 1,
                          /* expectedDocsReturnedSumOfSq */ 1,
                          false);

    assert.gt(queryStats[0].metrics.bytesRead.min, 0, queryStats);
    assert.gte(queryStats[0].metrics.readTimeMicros.min, 0, queryStats);
}

/**
 * TEST FIXTURES
 */

/**
 * Run callback function test on a fresh mongod instance. Toggling the server on/off for each
 * callback execution ensures that each execution of a command in a callback reads from disk instead
 * of the WT cache.
 *
 * @param {Mongo} setupConn - Connection object to the started mongod instance.
 * @param {String} collName - The name of the collection.
 * @param {Function} callback - The function that makes assertions about metrics.
 */
function runTestMongod(setupConn, collName, callback) {
    const conn = MongoRunner.runMongod(
        Object.assign(getQueryStatsServerParameters(),
                      {restart: true, cleanData: false, dbpath: setupConn.dbpath}));
    const coll = conn.getDB("test")[collName];
    callback(conn, coll);
    MongoRunner.stopMongod(conn);
}

/**
 * Run callback function test on a fresh replica set. Toggling the server on/off for each callback
 * execution ensures that each execution of a command in a callback reads from disk instead of the
 * WT cache.
 *
 * @param {ReplSetTest} rst - ReplSetTest instance.
 * @param {String} collName - The name of the collection.
 * @param {Function} callback - The function that makes assertions about metrics.
 */
function runTestReplicaSet(rst, collName, callback) {
    rst.startSet(undefined /* options */, true /* restart */);
    const conn = rst.getPrimary();
    const testDB = conn.getDB("test");
    const coll = testDB[collName];
    callback(conn, coll);
    rst.stopSet(15 /* signal, SIGTERM */, true /* forRestart */);
}

/**
 * Run callback function test on a fresh sharded cluster. Toggling the server on/off for each
 * callback execution ensures that each execution of a command in a callback reads from disk instead
 * of the WT cache.
 *
 * @param {ShardingTest} st - ShardingTest instance.
 * @param {String} collName - The name of the collection.
 * @param {Function} callback - The function that makes assertions about metrics.
 *
 * @warning Make sure you call ShardingTest stop method after calling this function. In order for
 * stop to run successfully, make sure the shards are currently running (i.e. don't call
 * stopAllShards immediately before stop).
 */
function runTestMongos(st, collName, callback) {
    st.stopAllShards({} /* options */, true /* forRestart */);
    st.restartAllShards();
    const conn = st.s;
    const coll = conn.getDB("test")[collName];
    callback(conn, coll);
}

/**
 * TESTS
 */

{
    const setupConn = MongoRunner.runMongod(getQueryStatsServerParameters());
    const setupColl = makeUnshardedCollection(setupConn);
    const collName = setupColl.getName();
    MongoRunner.stopMongod(setupConn);

    runTestMongod(setupConn, collName, runStorageStatsTestCount);
    runTestMongod(setupConn, collName, runStorageStatsTestDistinct);
    runTestMongod(setupConn, collName, runStorageStatsTestFind);
}

{
    const rst = new ReplSetTest({nodes: 3, nodeOptions: getQueryStatsServerParameters()});
    rst.startSet();
    rst.initiate();
    const collName = makeUnshardedCollection(rst.getPrimary()).getName();
    rst.stopSet(15 /* signal, SIGTERM */, true /* forRestart */);

    runTestReplicaSet(rst, collName, runStorageStatsTestCount);
    runTestReplicaSet(rst, collName, runStorageStatsTestDistinct);
    runTestReplicaSet(rst, collName, runStorageStatsTestFind);
}

{
    const st =
        new ShardingTest({shards: 2, other: {mongosOptions: getQueryStatsServerParameters()}});
    const testDB = st.s.getDB("test");
    assert.commandWorked(
        testDB.adminCommand({enableSharding: testDB.getName(), primaryShard: st.shard0.shardName}));
    const collName = makeShardedCollection(st).getName();

    runTestMongos(st, collName, runStorageStatsTestCount);
    runTestMongos(st, collName, runStorageStatsTestDistinct);
    runTestMongos(st, collName, runStorageStatsTestFind);

    st.stop();
}
