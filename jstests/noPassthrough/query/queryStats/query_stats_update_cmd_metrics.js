/**
 * This test confirms that query stats store metrics fields for an update command are correct when
 * inserting a new query stats store entry.
 *
 * @tags: [requires_fcv_83]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {
    assertAggregatedMetricsSingleExec,
    assertExpectedResults,
    getLatestQueryStatsEntry,
    getQueryStats,
    withQueryStatsEnabled,
} from "jstests/libs/query/query_stats_utils.js";

const collName = jsTestName();

const updateCommandObj = {
    update: collName,
    updates: [{q: {$or: [{v: {$lt: 3}}, {v: {$eq: 4}}]}, u: {v: 1000, updated: true}, multi: false}],
    comment: "running test command!!",
};

withQueryStatsEnabled(collName, (coll) => {
    const testDB = coll.getDB();
    if (FixtureHelpers.isMongos(testDB)) {
        // TODO SERVER-112050 Unskip this when we support sharded clusters for update.
        jsTest.log.info("Skipping test on sharded cluster");
        return;
    }

    assert.commandWorked(coll.insert([{v: 1}, {v: 2}, {v: 3}, {v: 4}]));

    assert.commandWorked(testDB.runCommand(updateCommandObj));

    if (!FeatureFlagUtil.isPresentAndEnabled(testDB, "QueryStatsUpdateCommand")) {
        const batch = getQueryStats(testDB.getMongo(), {collName: coll.getName()});
        assert.eq(batch, [], "expect no query stats for update when feature flag is off");

        // Running a non-update command with the feature flag off should still record query stats,
        // but without write metrics.
        const countCommandObj = {count: coll.getName(), query: {v: 1000}};
        assert.commandWorked(testDB.runCommand(countCommandObj));
        const countEntry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: coll.getName()});
        assert.eq(countEntry.key.queryShape.command, "count");
        assert(
            !countEntry.metrics.hasOwnProperty("writes"),
            "Expected no 'writes' field for non-write operation when feature flag is off. " +
                "Found metrics: " +
                tojson(countEntry.metrics),
        );
        return;
    }

    const firstEntry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: coll.getName()});
    assert.eq(firstEntry.key.queryShape.command, "update");

    assertAggregatedMetricsSingleExec(firstEntry, {
        keysExamined: 0,
        docsExamined: 1,
        hasSortStage: false,
        usedDisk: false,
        fromMultiPlanner: false,
        fromPlanCache: false,
        writes: {nMatched: 1, nUpserted: 0, nModified: 1, nDeleted: 0, nInserted: 0},
    });
    assertExpectedResults({
        results: firstEntry,
        expectedQueryStatsKey: firstEntry.key,
        expectedExecCount: 1,
        expectedDocsReturnedSum: 0,
        expectedDocsReturnedMax: 0,
        expectedDocsReturnedMin: 0,
        expectedDocsReturnedSumOfSq: 0,
    });
});
