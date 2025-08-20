/**
 * Test that checks a call to $rankFusion fails when search hybrid scoring feature
 * flags are turned off.
 */

// TODO SERVER-85426 Remove this test when 'featureFlagRankFusionBasic',
// 'featureFlagRankFusionFull' and 'featureFlagSearchHybridScoringFull' are removed.
{
    const conn = MongoRunner.runMongod({
        setParameter: {
            featureFlagRankFusionBasic: false,
            featureFlagRankFusionFull: false,
        },
    });
    assert.neq(null, conn, 'failed to start mongod');
    const testDB = conn.getDB('test');

    // Pipeline to run $rankFusion should fail without feature flag turned on.
    assert.commandFailedWithCode(
        testDB.runCommand({aggregate: 1, pipeline: [{$rankFusion: {}}], cursor: {}}),
        ErrorCodes.QueryFeatureNotAllowed);

    MongoRunner.stopMongod(conn);
}
{
    const conn = MongoRunner.runMongod({
        setParameter: {
            featureFlagRankFusionBasic: true,
            featureFlagRankFusionFull: false,
        },
    });
    assert.neq(null, conn, 'failed to start mongod');
    const testDB = conn.getDB('test');
    const coll = testDB.getCollection('coll');

    // Pipeline to run $rankFusion with scoreDetails should fail without full feature flag turned
    // on.
    assert.commandFailedWithCode(testDB.runCommand({
        aggregate: coll.getName(),
        pipeline: [{$rankFusion: {input: {pipelines: {a: [{$sort: {a: 1}}]}}, scoreDetails: true}}],
        cursor: {}
    }),
                                 ErrorCodes.QueryFeatureNotAllowed);

    MongoRunner.stopMongod(conn);
}
