/**
 * This test confirms that query stats for distinct are collected on views.
 *
 * @tags: [requires_fcv_81]
 */
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {
    assertAggregatedMetricsSingleExec,
    assertExpectedResults,
    getLatestQueryStatsEntry,
} from "jstests/libs/query/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const collName = jsTestName();
const viewName = "testView";

const distinctCommandObj = {
    distinct: viewName,
    key: "v"
};

function runViewTest(conn, coll) {
    const testDB = coll.getDB();

    coll.insert({v: 7});
    coll.insert({v: 3});
    coll.insert({v: 7});

    assert.commandWorked(testDB.createView(viewName, collName, []));

    assert.commandWorked(testDB.runCommand(distinctCommandObj));
    const firstEntry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: viewName});
    assert.eq(firstEntry.key.queryShape.command, "distinct", firstEntry);

    let parameterDB = FixtureHelpers.isMongos(testDB) ? conn.shard0 : testDB;
    const spillParameter = parameterDB.adminCommand({
        getParameter: 1,
        internalQueryEnableAggressiveSpillsInGroup: 1,
    });
    const aggressiveSpillsInGroup = spillParameter["internalQueryEnableAggressiveSpillsInGroup"];

    assertAggregatedMetricsSingleExec(firstEntry, {
        keysExamined: 0,
        docsExamined: 3,
        hasSortStage: false,
        usedDisk: aggressiveSpillsInGroup,
        fromMultiPlanner: false,
        fromPlanCache: false
    });
    assertExpectedResults(firstEntry,
                          firstEntry.key,
                          /* expectedExecCount */ 1,
                          /* expectedDocsReturnedSum */ 2,
                          /* expectedDocsReturnedMax */ 2,
                          /* expectedDocsReturnedMin */ 2,
                          /* expectedDocsReturnedSumOfSq */ 4,
                          /* getMores */ false);
}

const options = {
    setParameter: {internalQueryStatsRateLimit: -1},
};

{
    const conn = MongoRunner.runMongod(options);
    const testDB = conn.getDB("test");
    const coll = testDB[collName];
    coll.drop();

    runViewTest(conn, coll);
    MongoRunner.stopMongod(conn);
}

{
    const st = new ShardingTest({shards: 2, mongosOptions: options});
    const testDB = st.getDB("test");
    const coll = testDB[collName];
    st.shardColl(coll, {_id: 1}, {_id: 1});

    runViewTest(st, coll);
    st.stop();
}
