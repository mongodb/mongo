/**
 * This test confirms that query stats on mongos is not collected on distinct queries
 * where adding the metrics will cause the total result to exceed the
 * 16MB size limit imposed on BSONObjs.
 *
 * @tags: [featureFlagQueryStatsCountDistinct]
 */
import {getQueryStatsDistinctCmd, resetQueryStatsStore} from "jstests/libs/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const collName = jsTestName();

const distinctCommandObj = {
    distinct: collName,
    key: "v",
    includeQueryStatsMetrics: true
};

function setUpCollection(coll) {
    st.shardColl(coll, {_id: 1}, {_id: 1});

    // Insert documents that contain large strings so that the return value of distinct is just
    // under the 16 MB limit, but won't exceed the limit when metrics are appended.
    assert.commandWorked(coll.insert([
        {v: Array(1000000).join("%%%%%%")},
        {v: Array(1000000).join("!!!!!")},
        {v: Array(1000000).join("@@@@@")},
        {v: Array(154577).join("&&&&&")}
    ]));
}

const st = new ShardingTest({
    shards: 2,
    mongosOptions: {
        setParameter: {internalQueryStatsRateLimit: -1},
    }
});
const testDB = st.getDB("test");
const coll = testDB[collName];
setUpCollection(coll);

// Verify that query stats are collected when the result with metrics is just under the 16MB limit.
// In testing, this should create a result that is 3 bytes below the limit of kMaxResponseSize.
assert.commandWorked(testDB.runCommand(distinctCommandObj));
let distinctQueryStats = getQueryStatsDistinctCmd(testDB);
assert.eq(1, distinctQueryStats.length, distinctQueryStats);

// Reset the query stats store and insert a new document that will cause the result with metrics to
// exceed the limit.
resetQueryStatsStore(testDB.getMongo(), "1MB");
assert.commandWorked(coll.insert([{v: true}]));

// Verify that no query stats metrics were collected.
// In testing, this should create a result that is 1 byte above the limit of kMaxResponseSize.
assert.commandWorked(testDB.runCommand(distinctCommandObj));
let emptyDistinctQueryStats = getQueryStatsDistinctCmd(testDB);
assert.eq(0, emptyDistinctQueryStats.length, emptyDistinctQueryStats);

st.stop();
