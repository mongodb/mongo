/**
 * Test that checks if search hybrid scoring feature flag is present and enabled.
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

const conn = MongoRunner.runMongod({
    setParameter: {featureFlagSearchHybridScoring: true},
});
assert.neq(null, conn, 'failed to start mongod');
const test = conn.getDB('test');

// TODO SERVER-85426 Remove this test when 'featureFlagSearchHybridScoring' is removed.
assert(FeatureFlagUtil.isPresentAndEnabled(test, "SearchHybridScoring"),
       "featureFlagSearchHybridScoring is undefined or not enabled");

MongoRunner.stopMongod(conn);
