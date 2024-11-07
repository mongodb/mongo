/**
 * Test that checks a call to $rankFusion and $scoreFusion fail when search hybrid scoring feature
 * flags are turned off.
 */

// TODO SERVER-85426 Remove this test when 'featureFlagSearchHybridScoringPrerequisites' &
// 'featureFlagSearchHybridScoring' are removed.
{
    const conn = MongoRunner.runMongod({
        setParameter: {
            featureFlagSearchHybridScoringPrerequisites: false,
            featureFlagSearchHybridScoring: false
        },
    });
    assert.neq(null, conn, 'failed to start mongod');
    const testDB = conn.getDB('test');

    // Pipeline to run $rankFusion should fail without feature flag turned on.
    assert.commandFailedWithCode(
        testDB.runCommand({aggregate: 1, pipeline: [{$rankFusion: {}}], cursor: {}}),
        ErrorCodes.QueryFeatureNotAllowed);

    // Pipeline to run $scoreFusion should fail without feature flag turned on. Specified value of 1
    // for aggregate indicates a collection agnostic command. Specified cursor of default batch
    // size.
    assert.commandFailedWithCode(
        testDB.runCommand({aggregate: 1, pipeline: [{$scoreFusion: {}}], cursor: {}}),
        ErrorCodes.QueryFeatureNotAllowed);

    // Pipeline to run $score should fail without feature flag turned on. Specified value of 1
    // for aggregate indicates a collection agnostic command. Specified cursor of default batch
    // size.
    assert.commandFailedWithCode(
        testDB.runCommand({aggregate: 1, pipeline: [{$score: {}}], cursor: {}}),
        ErrorCodes.QueryFeatureNotAllowed);

    MongoRunner.stopMongod(conn);
}

// Also, confirm that when the prerequisites flag is set to true, but the main hybrid search flag is
// set to false, $score & $scoreFusion are still disabled.
{
    const conn = MongoRunner.runMongod({
        setParameter: {
            featureFlagSearchHybridScoringPrerequisites: true,
            featureFlagSearchHybridScoring: false
        },
    });
    assert.neq(null, conn, 'failed to start mongod');
    const testDB = conn.getDB('test');

    assert.commandFailedWithCode(
        testDB.runCommand({aggregate: 1, pipeline: [{$score: {}}], cursor: {}}),
        ErrorCodes.QueryFeatureNotAllowed);

    assert.commandFailedWithCode(
        testDB.runCommand({aggregate: 1, pipeline: [{$scoreFusion: {}}], cursor: {}}),
        ErrorCodes.QueryFeatureNotAllowed);

    MongoRunner.stopMongod(conn);
}
