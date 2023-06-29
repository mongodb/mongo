/**
 * Test that calls to read from telemetry store fail when feature flag is turned off.
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

// This test specifically tests error handling when the feature flag is not on.
// TODO SERVER-65800 this test can be removed when the feature flag is removed.
const conn = MongoRunner.runMongod();
const testDB = conn.getDB('test');
if (FeatureFlagUtil.isEnabled(testDB, "QueryStats")) {
    jsTestLog("Skipping test since query stats are enabled.");
    MongoRunner.stopMongod(conn);
    quit();
}

// Pipeline to read telemetry store should fail without feature flag turned on.
assert.commandFailedWithCode(
    testDB.adminCommand({aggregate: 1, pipeline: [{$queryStats: {}}], cursor: {}}),
    ErrorCodes.QueryFeatureNotAllowed);

// Pipeline, with a filter, to read telemetry store fails without feature flag turned on.
assert.commandFailedWithCode(testDB.adminCommand({
    aggregate: 1,
    pipeline: [{$queryStats: {}}, {$match: {"key.queryShape.find": {$eq: "###"}}}],
    cursor: {}
}),
                             ErrorCodes.QueryFeatureNotAllowed);

MongoRunner.stopMongod(conn);