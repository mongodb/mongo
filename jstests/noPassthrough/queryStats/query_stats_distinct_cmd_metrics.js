/**
 * This test confirms that query stats store metrics fields for a distinct command are correct
 * when inserting new query stats store entry.
 *
 * @tags: [featureFlagQueryStatsCountDistinct]
 */
import {
    assertAggregatedMetricsSingleExec,
    getLatestQueryStatsEntry,
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

    assert.commandWorked(testDB.runCommand(distinctCommandObj));
    const firstEntry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: coll.getName()});

    assert.eq(firstEntry.key.queryShape.command, "distinct");

    assertAggregatedMetricsSingleExec(firstEntry, {
        keysExamined: 0,
        docsExamined: 4,
        hasSortStage: false,
        usedDisk: false,
        fromMultiPlanner: false,
        fromPlanCache: false
    });
}, false);
