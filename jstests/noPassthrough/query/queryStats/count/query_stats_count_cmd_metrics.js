/**
 * This test confirms that query stats store metrics fields for a count command are correct
 * when inserting new query stats store entries.
 *
 * @tags: [requires_fcv_81]
 */

import {
    assertDropAndRecreateCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {
    assertAggregatedMetricsSingleExec,
    assertExpectedResults,
    getLatestQueryStatsEntry,
    getQueryStats,
    resetQueryStatsStore,
    withQueryStatsEnabled
} from "jstests/libs/query_stats_utils.js";

/**
 * Initialize collection for tests.
 */
function initializeCollection(db, collName) {
    assertDropAndRecreateCollection(db, collName);
    db[collName].insert({a: 1});
    db[collName].insert({a: 2});
    db[collName].insert({a: 3});
    db[collName].insert({a: 4});
}

/**
 * Test that a default count command generates the expected query stats.
 *
 * @param {object} db - The database object containing the collection and statistics.
 * @param {String} collName - The name of the collection.
 */
function testDefaultCountCommand(db, collName) {
    assert.commandWorked(db.runCommand({count: collName}));

    // Check that query stats metrics are properly updated.
    const firstEntry = getLatestQueryStatsEntry(db.getMongo(), {collname: collName});
    assert.eq("count", firstEntry.key.queryShape.command);
    assertAggregatedMetricsSingleExec(firstEntry, {
        keysExamined: 0,
        // docsExamined is 0 because empty query object means collection scan is unnecessary;
        // the query engine just uses the collection's number of records.
        docsExamined: 0,
        hasSortStage: false,
        usedDisk: false,
        fromMultiPlanner: false,
        fromPlanCache: false
    });
    assertExpectedResults(firstEntry,
                          firstEntry.key,
                          /* expectedExecCount */ 1,
                          /* expectedDocsReturnedSum */ 1,
                          /* expectedDocsReturnedMax */ 1,
                          /* expectedDocsReturnedMin */ 1,
                          /* expectedDocsReturnedSumOfSq */ 1,
                          /* getMores */ false);

    // Check that only one query stats entry is added.
    const stats = getQueryStats(db.getMongo(), {collName: collName});
    assert.eq(1, stats.length, stats);
}

/**
 * Test that a count command with a query parameter generates the expected query stats.
 *
 * @param {object} db - The database object containing the collection and statistics.
 * @param {String} collName - The name of the collection.
 */
function testCountCommandWithQuery(db, collName) {
    assert.commandWorked(db.runCommand({
        count: collName,
        query: {$or: [{a: {$lt: 3}}, {a: {$eq: 4}}]},
    }));

    // Check that query stats metrics are properly updated.
    const firstEntry = getLatestQueryStatsEntry(db.getMongo(), {collname: collName});
    assert.eq("count", firstEntry.key.queryShape.command);
    assertAggregatedMetricsSingleExec(firstEntry, {
        keysExamined: 0,
        docsExamined: 4,
        hasSortStage: false,
        usedDisk: false,
        fromMultiPlanner: false,
        fromPlanCache: false
    });
    assertExpectedResults(firstEntry,
                          firstEntry.key,
                          /* expectedExecCount */ 1,
                          /* expectedDocsReturnedSum */ 1,
                          /* expectedDocsReturnedMax */ 1,
                          /* expectedDocsReturnedMin */ 1,
                          /* expectedDocsReturnedSumOfSq */ 1,
                          /* getMores */ false);

    // Check that only one query stats entry is added.
    const stats = getQueryStats(db.getMongo(), {collName: collName});
    assert.eq(1, stats.length, stats);
}

/**
 * Test that a count command with an index generates the expected query stats.
 *
 * @param {object} db - The database object containing the collection and statistics.
 * @param {String} collName - The name of the collection.
 */
function testCountCommandWithIndex(db, collName) {
    assert.commandWorked(db[collName].createIndex({a: 1}));
    assert.commandWorked(db.runCommand({
        count: collName,
        query: {$or: [{a: {$lt: 3}}, {a: {$eq: 4}}]},
    }));

    // Check that query stats metrics are properly updated.
    const firstEntry = getLatestQueryStatsEntry(db.getMongo(), {collname: collName});
    assert.eq("count", firstEntry.key.queryShape.command, firstEntry);
    assertAggregatedMetricsSingleExec(firstEntry, {
        keysExamined: 4,
        docsExamined: 0,
        hasSortStage: false,
        usedDisk: false,
        fromMultiPlanner: false,
        fromPlanCache: false
    });
    assertExpectedResults(firstEntry,
                          firstEntry.key,
                          /* expectedExecCount */ 1,
                          /* expectedDocsReturnedSum */ 1,
                          /* expectedDocsReturnedMax */ 1,
                          /* expectedDocsReturnedMin */ 1,
                          /* expectedDocsReturnedSumOfSq */ 1,
                          /* getMores */ false);

    // Check that only one query stats entry is added.
    const stats = getQueryStats(db.getMongo(), {collName: collName});
    assert.eq(1, stats.length, stats);
}

/**
 * Run the tests for the count command metrics.
 */
function runTests(db, collName) {
    resetQueryStatsStore(db.getMongo(), "1MB");
    testDefaultCountCommand(db, collName);

    resetQueryStatsStore(db.getMongo(), "1MB");
    testCountCommandWithQuery(db, collName);

    resetQueryStatsStore(db.getMongo(), "1MB");
    testCountCommandWithIndex(db, collName);
}

withQueryStatsEnabled(jsTestName(), (coll) => {
    const testDB = coll.getDB();
    const collName = coll.getName();
    initializeCollection(testDB, collName);
    runTests(testDB, collName);
});
