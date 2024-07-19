/**
 * This test confirms that query stats is not collected on distinct explains and that
 * query stats store metrics fields for a distinct command are correct
 * when inserting new query stats store entry.
 *
 * @tags: [featureFlagQueryStatsCountDistinct]
 */
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {
    assertAggregatedMetricsSingleExec,
    assertExpectedResults,
    getLatestQueryStatsEntry,
    getQueryStatsDistinctCmd,
    withQueryStatsEnabled
} from "jstests/libs/query_stats_utils.js";

const collName = jsTestName();

const distinctCommandObj = {
    distinct: collName,
    key: "v",
    query: {$or: [{v: {$lt: 3}}, {v: {$eq: 4}}]},
    comment: "running test command!!",
};

withQueryStatsEnabled(collName, (coll) => {
    const testDB = coll.getDB();
    coll.insert({v: 1});
    coll.insert({v: 2});
    coll.insert({v: 3});
    coll.insert({v: 4});

    coll.insert({v: 2});
    coll.insert({v: 4});

    // Test that query stats are not collected for explains on distinct queries.
    coll.explain("executionStats").distinct("v");
    let emptyDistinctQueryStats = getQueryStatsDistinctCmd(testDB);
    assert.eq(0, emptyDistinctQueryStats.length);

    assert.commandWorked(testDB.runCommand(distinctCommandObj));
    const firstEntry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: coll.getName()});
    assert.eq(firstEntry.key.queryShape.command, "distinct");

    // TODO SERVER-90651: remove if statement, since metrics should be identical
    // after enabling data bearing nodes. keysExamined and docsExamined will be 0
    // until data bearing nodes is enabled.
    if (FixtureHelpers.isMongos(testDB)) {
        assertAggregatedMetricsSingleExec(firstEntry, {
            keysExamined: 0,
            docsExamined: 0,
            hasSortStage: false,
            usedDisk: false,
            fromMultiPlanner: false,
            fromPlanCache: false
        });
    } else {
        assertAggregatedMetricsSingleExec(firstEntry, {
            keysExamined: 0,
            docsExamined: 6,
            hasSortStage: false,
            usedDisk: false,
            fromMultiPlanner: false,
            fromPlanCache: false
        });
    }
    assertExpectedResults(firstEntry,
                          firstEntry.key,
                          /* expectedExecCount */ 1,
                          /* expectedDocsReturnedSum */ 3,
                          /* expectedDocsReturnedMax */ 3,
                          /* expectedDocsReturnedMin */ 3,
                          /* expectedDocsReturnedSumOfSq */ 9,
                          /* getMores */ false);
});
