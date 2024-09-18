/**
 * Test that checks a call to $rankFusion fails when search hybrid scoring feature flag is turned
 * off.
 */

// TODO SERVER-85426 Remove this test when 'featureFlagSearchHybridScoring' is removed.
const conn = MongoRunner.runMongod({
    setParameter: {featureFlagSearchHybridScoring: false},
});
assert.neq(null, conn, 'failed to start mongod');
const testDB = conn.getDB('test');

// Pipeline to run $rankFusion should fail without feature flag turned on.
assert.commandFailedWithCode(
    testDB.runCommand({aggregate: 1, pipeline: [{$rankFusion: {}}], cursor: {}}),
    ErrorCodes.QueryFeatureNotAllowed);

// Pipeline to run $scoreFusion should fail without feature flag turned on. Specified value of 1
// for aggregate indicates a collection agnostic command. Specified cursor of default batch size.
assert.commandFailedWithCode(
    testDB.runCommand({aggregate: 1, pipeline: [{$scoreFusion: {}}], cursor: {}}),
    ErrorCodes.QueryFeatureNotAllowed);

// Pipeline to run $score should fail without feature flag turned on. Specified value of 1
// for aggregate indicates a collection agnostic command. Specified cursor of default batch size.
assert.commandFailedWithCode(
    testDB.runCommand({aggregate: 1, pipeline: [{$score: {}}], cursor: {}}),
    ErrorCodes.QueryFeatureNotAllowed);

MongoRunner.stopMongod(conn);
