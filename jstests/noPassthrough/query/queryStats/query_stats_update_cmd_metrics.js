/**
 * This test confirms that query stats store metrics fields for an update command (where the
 * update modification is specified as a replacement document or pipeline) are correct when
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

/**
 * Test that a replacement update generates the expected query stats.
 */
function testReplacementUpdateMetrics(coll) {
    const testDB = coll.getDB();
    const replacementUpdateCommandObj = {
        update: collName,
        updates: [{q: {$or: [{v: {$lt: 3}}, {v: {$eq: 4}}]}, u: {v: 1000, updated: true}, multi: false}],
        comment: "running replacement update!!",
    };

    assert.commandWorked(testDB.runCommand(replacementUpdateCommandObj));

    const entry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: coll.getName()});
    assert.eq(entry.key.queryShape.command, "update");

    assertAggregatedMetricsSingleExec(entry, {
        keysExamined: 0,
        docsExamined: 1,
        hasSortStage: false,
        usedDisk: false,
        fromMultiPlanner: false,
        fromPlanCache: false,
        writes: {nMatched: 1, nUpserted: 0, nModified: 1, nDeleted: 0, nInserted: 0},
    });
    assertExpectedResults({
        results: entry,
        expectedQueryStatsKey: entry.key,
        expectedExecCount: 1,
        expectedDocsReturnedSum: 0,
        expectedDocsReturnedMax: 0,
        expectedDocsReturnedMin: 0,
        expectedDocsReturnedSumOfSq: 0,
    });
}

/**
 * Test that a modifier update generates the expected query stats.
 */
function testModifierUpdateMetrics(coll) {
    const testDB = coll.getDB();
    const modifierUpdateCommandObj = {
        update: collName,
        updates: [
            {
                q: {}, // Should match all docs in collection.
                u: {$set: {v: "newValue", documentUpdated: true, count: 42}},
                multi: true,
            },
        ],
        comment: "running modifier update!!",
    };

    assert.commandWorked(testDB.runCommand(modifierUpdateCommandObj));

    const entry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: coll.getName()});
    assert.eq(entry.key.queryShape.command, "update");

    assertAggregatedMetricsSingleExec(entry, {
        keysExamined: 0,
        docsExamined: 8,
        hasSortStage: false,
        usedDisk: false,
        fromMultiPlanner: false,
        fromPlanCache: false,
        writes: {nMatched: 8, nUpserted: 0, nModified: 8, nDeleted: 0, nInserted: 0},
    });

    assertExpectedResults({
        results: entry,
        expectedQueryStatsKey: entry.key,
        expectedExecCount: 1,
        expectedDocsReturnedSum: 0,
        expectedDocsReturnedMax: 0,
        expectedDocsReturnedMin: 0,
        expectedDocsReturnedSumOfSq: 0,
    });
}

/**
 * Test that a pipeline update generates the expected query stats.
 */
function testPipelineUpdateMetrics(coll) {
    const testDB = coll.getDB();
    const pipelineUpdateCommandObj = {
        update: collName,
        updates: [
            {
                q: {}, // Should match all docs in collection.
                u: [
                    {$set: {v: "$$newValue", pipelineUpdated: true, count: 42}},
                    {$unset: "oldField"},
                    {$replaceWith: {newDoc: "$$ROOT", timestamp: "$$NOW", processed: true}},
                ],
                c: {newValue: 3000},
                multi: true,
            },
        ],
        comment: "running pipeline update!!",
    };

    assert.commandWorked(testDB.runCommand(pipelineUpdateCommandObj));

    const entry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: coll.getName()});
    assert.eq(entry.key.queryShape.command, "update");

    assertAggregatedMetricsSingleExec(entry, {
        keysExamined: 0,
        docsExamined: 8,
        hasSortStage: false,
        usedDisk: false,
        fromMultiPlanner: false,
        fromPlanCache: false,
        writes: {nMatched: 8, nUpserted: 0, nModified: 8, nDeleted: 0, nInserted: 0},
    });

    assertExpectedResults({
        results: entry,
        expectedQueryStatsKey: entry.key,
        expectedExecCount: 1,
        expectedDocsReturnedSum: 0,
        expectedDocsReturnedMax: 0,
        expectedDocsReturnedMin: 0,
        expectedDocsReturnedSumOfSq: 0,
    });
}

function testFeatureFlagOff(coll) {
    const testDB = coll.getDB();
    const updateCommandObj = {
        update: collName,
        updates: [{q: {$or: [{v: {$lt: 3}}, {v: {$eq: 4}}]}, u: {v: 1000, updated: true}, multi: false}],
        comment: "running update with feature flag off!!",
    };

    assert.commandWorked(testDB.runCommand(updateCommandObj));

    const batch = getQueryStats(testDB.getMongo(), {collName: coll.getName()});
    assert.eq(batch, [], "expect no query stats for update when feature flag is off");

    // Running a non-update command with the feature flag off should still record query stats,
    // but without write metrics.
    const countCommandObj = {count: collName, query: {v: 1000}};
    assert.commandWorked(testDB.runCommand(countCommandObj));
    const countEntry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: collName});
    assert.eq(countEntry.key.queryShape.command, "count");
    assert(
        !countEntry.metrics.hasOwnProperty("writes"),
        "Expected no 'writes' field for non-write operation when feature flag is off. " +
            "Found metrics: " +
            tojson(countEntry.metrics),
    );
}

function resetCollection(coll) {
    coll.drop();
    assert.commandWorked(coll.insert([{v: 1}, {v: 2}, {v: 3}, {v: 4}, {v: 5}, {v: 6}, {v: 7}, {v: 8}]));
}

withQueryStatsEnabled(collName, (coll) => {
    const testDB = coll.getDB();
    if (FixtureHelpers.isMongos(testDB)) {
        // TODO SERVER-112050 Unskip this when we support sharded clusters for update.
        jsTest.log.info("Skipping test on sharded cluster");
        return;
    }

    resetCollection(coll);

    if (!FeatureFlagUtil.isPresentAndEnabled(testDB, "QueryStatsUpdateCommand")) {
        testFeatureFlagOff(coll);
        return;
    }

    jsTest.log.info("Testing replacement update metrics");
    testReplacementUpdateMetrics(coll);
    resetCollection(coll);

    jsTest.log.info("Testing modifier update metrics");
    testModifierUpdateMetrics(coll);
    resetCollection(coll);

    jsTest.log.info("Testing pipeline update metrics");
    testPipelineUpdateMetrics(coll);
});
