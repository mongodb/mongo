/**
 * Tests that when a QueryFeatureNotAllowed error is hit when pulling results from query stats,
 * we skip asserting, and just ignore the entry, despite being in debug build mode,
 * or the internalQueryStatsErrorsAreCommandFatal is set.
 *
 * Normally, when processing a $queryStats command, if an entry in the query stats store
 * produces an error when retrieving and processing for an output of the stage,
 * it is ignored and logged.
 *
 * However, when running in debug mode, or the 'internalQueryStatsErrorsAreCommandFatal' option
 * is set, the stage will raise an assertion, so that a test failure can be caught and investigated
 * (suggesting an error in $queryStats that should be patched).
 *
 * Furthermore, when running in debug mode, or the 'internalQueryStatsErrorsAreCommandFatal' is set,
 * if the error that is encountered is a QueryFeatureNotAllowed, we should not raise such assertion,
 * because it does not suggest an error in $queryStats that should be investigated.
 * This is because we know this type of error is generated only in the following situation:
 * - The FCV is high enough to run some sort of query feature that needs it.
 * - (implicitly this query shape is logged in the query stats store)
 * - Later, the FCV drops below the required version needed to run the aforementioned query.
 * - $queryStats is then run, and this query shape in the store cannot be properly parsed/processed.
 *
 * In this test we simulate this error case with fail-point
 * 'queryStatsGenerateQueryFeatureNotAllowedError' to not rely on any specific query that needs
 * a specific FCV to run, so that this test can be resilient across major version releases and
 * feature flag removals.
 */

const collName = jsTestName();

const options = {
    setParameter: {
        internalQueryStatsErrorsAreCommandFatal: 1,
        internalQueryStatsRateLimit: -1,
    },
};

const conn = MongoRunner.runMongod(options);
const testDB = conn.getDB("test");
var coll = testDB[collName];
coll.drop();

// Run a simple query that does not need a particular FCV to run,
// so that there is at least one query shape stored in the query stats store.
assert.commandWorked(conn.adminCommand(
    {aggregate: coll.getName(), pipeline: [{$project: {_id: 1, x: "x"}}], cursor: {}}));

// Now enable the fail point that simulates that the query feature is not allowed for
// the query shape saved in the query stats store.
assert.commandWorked(testDB.adminCommand(
    {'configureFailPoint': "queryStatsGenerateQueryFeatureNotAllowedError", 'mode': 'alwaysOn'}));

// Now when requesting the $queryStats, we should receive a QueryFeatureNotEnabled error,
// but despite 'internalQueryStatsErrorsAreCommandFatal' and (potentially) being in debug mode,
// the query should still succeed.
assert.commandWorked(conn.adminCommand({aggregate: 1, pipeline: [{$queryStats: {}}], cursor: {}}));

// Now assert that no results were returned by $queryStats.
// We know there should be only one key in the store,
// and it should be skipped over without failing the query.
let results = testDB.getSiblingDB("admin")
                  .aggregate([
                      {$queryStats: {}},
                  ])
                  .toArray();
assert.eq(results.length, 0);

// Now turn on the fail point that will also generate an error in $queryStats,
// but not the one that should be skipped over, so we can ensure that $queryStats
// still fails when we expect it to.
assert.commandWorked(testDB.adminCommand(
    {'configureFailPoint': "queryStatsGenerateQueryFeatureNotAllowedError", 'mode': 'off'}));
assert.commandWorked(testDB.adminCommand(
    {'configureFailPoint': "queryStatsFailToReparseQueryShape", 'mode': 'alwaysOn'}));
assert.commandFailedWithCode(
    conn.adminCommand({aggregate: 1, pipeline: [{$queryStats: {}}], cursor: {}}),
    ErrorCodes.QueryStatsFailedToRecord);

MongoRunner.stopMongod(conn);
