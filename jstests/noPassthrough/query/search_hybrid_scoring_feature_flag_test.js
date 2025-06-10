/**
 * Test that checks a call to $rankFusion, $scoreFusion and $score fail when search hybrid scoring
 * feature flags are turned off.
 */

import {
    assertCreateCollection,
    assertDropCollection
} from "jstests/libs/collection_drop_recreate.js";

// TODO SERVER-85426 Remove this test when 'featureFlagRankFusionBasic',
// 'featureFlagRankFusionFull' and 'featureFlagSearchHybridScoringFull' are removed.
{
    const conn = MongoRunner.runMongod({
        setParameter: {
            featureFlagRankFusionBasic: false,
            featureFlagRankFusionFull: false,
            featureFlagSearchHybridScoringFull: false,
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

// Confirm that when only the first rankFusion flag is set to true, $score & $scoreFusion are
// disabled.
{
    const conn = MongoRunner.runMongod({
        setParameter: {
            featureFlagRankFusionBasic: true,
            featureFlagRankFusionFull: false,
            featureFlagSearchHybridScoringFull: false
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

    assert.commandFailedWithCode(
        testDB.runCommand({aggregate: 1, pipeline: [{$score: {}}], cursor: {}}),
        ErrorCodes.QueryFeatureNotAllowed);

    assert.commandFailedWithCode(
        testDB.runCommand({aggregate: 1, pipeline: [{$scoreFusion: {}}], cursor: {}}),
        ErrorCodes.QueryFeatureNotAllowed);

    // Running $rankFusion against a view only works when 'featureFlagSearchHybridScoringFull' is
    // enabled.
    assertCreateCollection(testDB, "test_coll");
    assert.commandWorked(testDB.createView("test_view", "test_coll", [{$match: {a: 1}}]));

    assert.commandFailedWithCode(testDB.runCommand({
        aggregate: "test_view",
        pipeline: [{$rankFusion: {input: {pipelines: {a: [{$sort: {a: 1}}]}}}}],
        cursor: {}
    }),
                                 ErrorCodes.OptionNotSupportedOnView);

    assertDropCollection(testDB, "test_view");
    assertDropCollection(testDB, "test_coll");

    MongoRunner.stopMongod(conn);
}

// Confirm that when only the main hybrid search flag is disabled, $scoreFusion is still disabled,
// same as $rankFusion on views.
{
    const conn = MongoRunner.runMongod({
        setParameter: {
            featureFlagRankFusionBasic: true,
            featureFlagRankFusionFull: true,
            featureFlagSearchHybridScoringFull: false
        },
    });
    assert.neq(null, conn, 'failed to start mongod');
    const testDB = conn.getDB('test');

    assert.commandFailedWithCode(
        testDB.runCommand({aggregate: 1, pipeline: [{$scoreFusion: {}}], cursor: {}}),
        ErrorCodes.QueryFeatureNotAllowed);

    assertCreateCollection(testDB, "test_coll");
    assert.commandWorked(testDB.createView("test_view", "test_coll", [{$match: {a: 1}}]));

    // Running $rankFusion against a view only works when 'featureFlagSearchHybridScoringFull' is
    // enabled.
    assert.commandFailedWithCode(testDB.runCommand({
        aggregate: "test_view",
        pipeline: [{$rankFusion: {input: {pipelines: {a: [{$sort: {a: 1}}]}}}}],
        cursor: {}
    }),
                                 ErrorCodes.OptionNotSupportedOnView);

    assertDropCollection(testDB, "test_view");
    assertDropCollection(testDB, "test_coll");

    MongoRunner.stopMongod(conn);
}
