/**
 * Tests that the stub aggregation stages included in aggregation_stage_fallback_parsers.json
 * model unloaded extensions by returning the specified error message rather than the generic
 * "unrecognized stage" error.
 *
 * Also tests the feature flag toggle behavior for $testFoo:
 *   - When featureFlagExtensionStubParsers is enabled: use the primary parser (extension's implementation)
 *   - When featureFlagExtensionStubParsers is disabled: use the fallback stub parser (throws error)
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";
import {checkPlatformCompatibleWithExtensions, withExtensions} from "jstests/noPassthrough/libs/extension_helpers.js";

checkPlatformCompatibleWithExtensions();

/**
 * Test basic stub parser behavior for $stubStage (no feature flag).
 * $stubStage should always error since there is no extension that provides it.
 */
function testStubStageAlwaysErrors(db) {
    const coll = db[jsTestName()];

    // Test that using $stubStage outputs the expected error.
    let pipeline = [{$stubStage: {}}];
    assertErrorCode(coll, pipeline, 10918500);

    // Test $stubStage in the middle of a complex pipeline.
    pipeline = [{$match: {_id: {$in: [0, 1]}}}, {$stubStage: {}}, {$sort: {x: -1}}];
    assertErrorCode(coll, pipeline, 10918500);
}

/**
 * Test $testFoo behavior based on whether the extension is loaded and the feature flag is enabled.
 *
 * | Extension Loaded? | Feature Flag | Behavior                   | Outcome |
 * |-------------------|--------------|----------------------------|---------|
 * | Yes               | On           | Primary parser (extension) | Works   |
 * | Yes               | Off          | Fallback stub parser       | Error   |
 * | No                | On           | Fallback stub parser       | Error   |
 * | No                | Off          | Fallback stub parser       | Error   |
 */
function testTestFooWithFeatureFlag(db, fooExtensionLoaded, featureFlagEnabled) {
    const coll = db[jsTestName()];
    const pipeline = [{$testFoo: {}}];

    if (fooExtensionLoaded && featureFlagEnabled) {
        assert.commandWorked(db.runCommand({aggregate: coll.getName(), pipeline: pipeline, cursor: {}}));
    } else {
        assertErrorCode(coll, pipeline, 10918500);
    }
}

/**
 * Toggle featureFlagExtensionStubParsers and run tests for $testFoo.
 */
function runFeatureFlagToggleTests(conn, fooExtensionLoaded) {
    const adminDb = conn.getDB("admin");
    const testDb = conn.getDB("test");

    // Test with feature flag DISABLED.
    assert.commandWorked(adminDb.runCommand({setParameter: 1, featureFlagExtensionStubParsers: false}));
    testTestFooWithFeatureFlag(testDb, fooExtensionLoaded, false);

    // Test with feature flag ENABLED.
    assert.commandWorked(adminDb.runCommand({setParameter: 1, featureFlagExtensionStubParsers: true}));
    testTestFooWithFeatureFlag(testDb, fooExtensionLoaded, true);
}

// Test without any extensions loaded.
withExtensions({}, (conn) => {
    const testDb = conn.getDB("test");

    testStubStageAlwaysErrors(testDb);
    runFeatureFlagToggleTests(conn, false);
});

// Test with the foo extension loaded.
withExtensions({"libfoo_mongo_extension.so": {}}, (conn) => {
    const testDb = conn.getDB("test");

    testStubStageAlwaysErrors(testDb);
    runFeatureFlagToggleTests(conn, true);
});
