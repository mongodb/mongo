/**
 * This test confirms that query stats store metrics fields for a
 * distinct command are correct when inserting a new query stats store entry.
 *
 * @tags: [requires_fcv_81]
 */
import {
    assertAggregatedMetricsSingleExec,
    assertExpectedResults,
    getLatestQueryStatsEntry,
    withQueryStatsEnabled
} from "jstests/libs/query/query_stats_utils.js";

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

    assert.commandWorked(testDB.runCommand(distinctCommandObj));
    const firstEntry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: coll.getName()});
    assert.eq(firstEntry.key.queryShape.command, "distinct");

    assertAggregatedMetricsSingleExec(firstEntry, {
        keysExamined: 0,
        docsExamined: 6,
        hasSortStage: false,
        usedDisk: false,
        fromMultiPlanner: false,
        fromPlanCache: false
    });
    assertExpectedResults(firstEntry,
                          firstEntry.key,
                          /* expectedExecCount */ 1,
                          /* expectedDocsReturnedSum */ 3,
                          /* expectedDocsReturnedMax */ 3,
                          /* expectedDocsReturnedMin */ 3,
                          /* expectedDocsReturnedSumOfSq */ 9,
                          /* getMores */ false);
});
