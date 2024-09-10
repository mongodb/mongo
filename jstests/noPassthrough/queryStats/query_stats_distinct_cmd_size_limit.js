/**
 * This test confirms that query stats on mongos is not collected on distinct queries
 * where adding the metrics will cause the total result to exceed the
 * 16MB size limit imposed on BSONObjs.
 *
 * @tags: [requires_fcv_81]
 */
import {getQueryStatsDistinctCmd, resetQueryStatsStore} from "jstests/libs/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const collName = jsTestName();
const viewName = "testView";

const distinctCommandObj = {
    distinct: collName,
    key: "v"
};
const distinctViewCommandObj = {
    distinct: viewName,
    key: "v"
};

function setUpCollection(coll) {
    coll.drop();
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
function setUpCollectionForView(coll) {
    coll.drop();
    st.shardColl(coll, {_id: 1}, {_id: 1});

    // Insert documents that contain large strings so that the total BSONObj length of
    // the agg cursor for the view is just under the 16 MB limit.
    assert.commandWorked(coll.insert([
        {v: Array(1000000).join("%%%%%%")},
        {v: Array(1000000).join("!!!!!")},
        {v: Array(1000000).join("@@@@@")},
        {v: Array(154577).join("&&&&&")},
        {v: Array(4048).join("*****")}
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

{
    setUpCollection(coll);

    // Verify that query stats are collected when the result with metrics is just under the 16MB
    // limit. In testing, this should create a result that is 3 bytes below the limit of
    // kMaxResponseSize.
    assert.commandWorked(testDB.runCommand(distinctCommandObj));
    let distinctQueryStats = getQueryStatsDistinctCmd(testDB);
    assert.eq(1, distinctQueryStats.length, distinctQueryStats);

    // Reset the query stats store and insert a new document that will cause the result with metrics
    // to exceed the limit.
    resetQueryStatsStore(testDB.getMongo(), "1MB");
    assert.commandWorked(coll.insert([{v: true}]));

    // Verify that no query stats metrics were collected.
    // In testing, this should create a result that is 1 byte above the limit of kMaxResponseSize.
    assert.commandWorked(testDB.runCommand(distinctCommandObj));
    let emptyDistinctQueryStats = getQueryStatsDistinctCmd(testDB);
    assert.eq(0, emptyDistinctQueryStats.length, emptyDistinctQueryStats);
}

resetQueryStatsStore(testDB.getMongo(), "1MB");

// TODO SERVER-93216: Update this test's behavior once the bug is fixed. After the fix, enabling
// query stats should not cause a previously working distinct command on a view, therefore an
// aggregation command, to fail. For views, it should follow similar behavior as distinct works
// in the test above, where it won't append metrics if they cause the command to exceed the limit.

// For views running aggregation pipelines, because there is not a size limit check before
// adding the metrics to the cursor, the command will either succeed with query stats or fail
// entirely.
{
    setUpCollectionForView(coll);
    assert.commandWorked(testDB.createView(viewName, collName, []));

    // Verify that query stats are collected when the total result length is just under the BSONObj
    // limit.
    assert.commandWorked(testDB.runCommand(distinctViewCommandObj));
    let distinctQueryStats = getQueryStatsDistinctCmd(testDB);
    assert.eq(1, distinctQueryStats.length, distinctQueryStats);

    // Insert a new document that will cause the result of the query to exceed the BSONObj limit.
    assert.commandWorked(coll.insert([{v: '('}]));

    // Verify that the entire command fails due to the BSONObj exceeding the size limit.
    assert.commandFailedWithCode(
        testDB.runCommand(distinctViewCommandObj), 10334, "BSONObjectTooLarge");
}

// TODO SERVER-93216: Update this test's behavior once the bug is fixed. After the fix, enabling
// query stats should not cause a previously working distinct command on a view, therefore an
// aggregation command, to fail. For views, it means that enabling query stats should not change
// if a distinct command succeeds or not.

// For views running aggregation pipelines, confirm that enabling query stats can cause a
// command to error out when it previously worked without query stats enabled.
{
    // Disable query stats at first for this test
    assert.commandWorked(
        testDB.getMongo().adminCommand({setParameter: 1, internalQueryStatsCacheSize: "0MB"}));

    setUpCollectionForView(coll);
    assert.commandWorked(coll.insert([{v: Array(191).join("-")}]));
    assert.commandWorked(testDB.createView(viewName, collName, []));

    // Verify that the command works when query stats are not collected
    assert.commandWorked(testDB.runCommand(distinctViewCommandObj));

    // Turn query stats back on
    assert.commandWorked(
        testDB.getMongo().adminCommand({setParameter: 1, internalQueryStatsCacheSize: "1MB"}));

    // Verify that the entire command fails due to the BSONObj exceeding the size limit.
    assert.commandFailedWithCode(
        testDB.runCommand(distinctViewCommandObj), 10334, "BSONObjectTooLarge");
}

st.stop();
