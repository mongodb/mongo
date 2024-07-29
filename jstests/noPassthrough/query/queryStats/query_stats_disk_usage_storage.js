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
    getDistinctQueryStatsKey,
    getFindQueryStatsKey,
    getQueryStatsDistinctCmd,
} from "jstests/libs/query_stats_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

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

function runStorageStatsTestFind(conn, coll) {
    const cmd = {find: coll.getName(), filter: {}, batchSize: 100};
    const shape = {filter: {}};
    const expectedDocs = 7;

    const queryStatsKey = getFindQueryStatsKey(conn, coll.getName(), shape);
    clearPlanCacheAndQueryStatsStore(conn, coll);

    const queryStats = exhaustCursorAndGetQueryStats(conn, coll, cmd, queryStatsKey, expectedDocs);

    // The assertions in exhaustCursorAndGetQueryStats will check that the metrics are
    // sensible (e.g., max >= min), so we'll just assert that the minimum > 0.
    assert.gt(queryStats.metrics.bytesRead.min, 0);
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

// The options passed to runMongod, rst.startSet(), and others, sometimes get modified by those
// functions. So instead of having a single global constant that we pass around (risking
// modification), we return the defaults from a function.
function defaultOptions() {
    return {setParameter: {internalQueryStatsRateLimit: -1}};
}

{
    const setupConn = MongoRunner.runMongod(defaultOptions());
    const setupColl = makeUnshardedCollection(setupConn);
    const collName = setupColl.getName();
    MongoRunner.stopMongod(setupConn);

    const conn = MongoRunner.runMongod(Object.assign(
        defaultOptions(), {restart: true, cleanData: false, dbpath: setupConn.dbpath}));
    const coll = conn.getDB("test")[collName];
    runStorageStatsTestFind(conn, coll);
    runStorageStatsTestDistinct(conn, coll);
    MongoRunner.stopMongod(conn);
}

{
    const rst = new ReplSetTest({nodes: 3, nodeOptions: defaultOptions()});

    rst.startSet();
    rst.initiate();
    const collName = makeUnshardedCollection(rst.getPrimary()).getName();

    rst.stopSet(15 /* signal, SIGTERM */, true /* forRestart */);
    rst.startSet(undefined /* options */, true /* restart */);

    const conn = rst.getPrimary();
    const coll = conn.getDB("test")[collName];
    runStorageStatsTestFind(conn, coll);
    runStorageStatsTestDistinct(conn, coll);

    rst.stopSet();
}

{
    const st = new ShardingTest({shards: 2, other: {mongosOptions: defaultOptions()}});

    const testDB = st.s.getDB("test");
    assert.commandWorked(
        testDB.adminCommand({enableSharding: testDB.getName(), primaryShard: st.shard0.shardName}));

    const collName = makeShardedCollection(st).getName();

    st.stopAllShards({} /* options */, true /* forRestart */);
    st.restartAllShards();

    const conn = st.s;
    const coll = conn.getDB("test")[collName];
    runStorageStatsTestFind(conn, coll);
    runStorageStatsTestDistinct(conn, coll);

    st.stop();
}
