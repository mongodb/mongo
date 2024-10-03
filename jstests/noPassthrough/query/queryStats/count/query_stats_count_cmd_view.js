/**
 * This tests confirms that a count command on a view is implemented as an aggregation but stored as
 * a count command query shape, and that explains are not collected.
 *
 * @tags: [requires_fcv_81]
 */
import {
    assertAggregatedMetricsSingleExec,
    assertExpectedResults,
    getLatestQueryStatsEntry,
    getQueryStats,
    resetQueryStatsStore,
    withQueryStatsEnabled,
} from "jstests/libs/query/query_stats_utils.js";

withQueryStatsEnabled(jsTestName(), (coll) => {
    // Insert documents into a collection.
    coll.insert({a: 1, b: 1});
    coll.insert({a: 2, b: 1});
    coll.insert({a: 3, b: 1});
    coll.insert({a: 3, b: 2});
    coll.insert({a: 4, b: 1});

    // Create a view on the data.
    const viewsDB = coll.getDB("view_count");
    resetQueryStatsStore(viewsDB.getMongo(), "1MB");
    const collName = coll.getName();
    assert.commandWorked(viewsDB.runCommand({create: "identityView", viewOn: collName}));

    const command = {count: "identityView", query: {a: 3}};

    // Check that collection is disabled for explain.
    assert.commandWorked(viewsDB.runCommand({explain: command}));
    const explainStats = getQueryStats(viewsDB.getMongo());
    assert.eq(explainStats.length, 0, explainStats);

    // Delete entry from $queryStats command.
    resetQueryStatsStore(viewsDB.getMongo(), "1MB");

    // Check that query statistics are stored for a count command, not an aggregate command.
    assert.commandWorked(viewsDB.runCommand(command));
    const stats = getQueryStats(viewsDB.getMongo());
    assert.eq(1, stats.length, stats);
    const entry = getLatestQueryStatsEntry(viewsDB.getMongo(), {collName: "identityView"});
    assert.eq("count", entry.key.queryShape.command, entry);

    assertAggregatedMetricsSingleExec(entry, {
        keysExamined: 0,
        docsExamined: 5,
        hasSortStage: false,
        // Don't validate the value of 'usedDisk' here. On some variants this can actually be true,
        // but this test is not concerned with validating that.
        usedDisk: entry.metrics.usedDisk["true"] > 0,
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
