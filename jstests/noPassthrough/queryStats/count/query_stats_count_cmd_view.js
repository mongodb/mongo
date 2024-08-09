/**
 * This tests confirms that a count command on a view is implemented as an aggregation but stored as
 * a count command query shape.
 *
 * @tags: [featureFlagQueryStatsCountDistinct]
 */

import {
    assertAggregatedMetricsSingleExec,
    assertExpectedResults,
    getLatestQueryStatsEntry,
    getQueryStats,
    resetQueryStatsStore,
    withQueryStatsEnabled,
} from "jstests/libs/query_stats_utils.js";

withQueryStatsEnabled(jsTestName(), (coll) => {
    // Insert documents into a collection.
    coll.insert({a: 1});
    coll.insert({a: 2});
    coll.insert({a: 3});
    coll.insert({a: 4});

    // Create a view on the data.
    const viewsDB = coll.getDB("view_count");
    resetQueryStatsStore(viewsDB.getMongo(), "1MB");
    const collName = coll.getName();
    assert.commandWorked(viewsDB.runCommand({create: "identityView", viewOn: collName}));

    const command = {count: "identityView", query: {a: 3}};

    // Check that query statistics are stored for a count command, not an aggregate command.
    assert.commandWorked(viewsDB.runCommand(command));
    const stats = getQueryStats(viewsDB.getMongo());
    assert.eq(1, stats.length, stats);
    const entry = getLatestQueryStatsEntry(viewsDB.getMongo(), {collName: "identityView"});
    assert.eq("count", entry.key.queryShape.command, entry);
    assertAggregatedMetricsSingleExec(entry, {
        keysExamined: 0,
        docsExamined: 4,
        hasSortStage: false,
        usedDisk: false,
        fromMultiPlanner: false,
        fromPlanCache: false
    });
    assertExpectedResults(entry,
                          entry.key,
                          /* expectedExecCount */ 1,
                          /* expectedDocsReturnedSum */ 1,
                          /* expectedDocsReturnedMax */ 1,
                          /* expectedDocsReturnedMin */ 1,
                          /* expectedDocsReturnedSumOfSq */ 1,
                          /* getMores */ false);
});
