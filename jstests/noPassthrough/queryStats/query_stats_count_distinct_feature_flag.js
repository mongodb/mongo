/**
 * Test that checks the value of the query stats count and distinct feature flag.
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

// TODO SERVER-76892 this test can be removed when the feature flag is removed.
const conn = MongoRunner.runMongod({
    setParameter: {featureFlagQueryStatsCountDistinct: false},
});
assert.neq(null, conn, 'failed to start mongod');
const testDB = conn.getDB('test');

assert(FeatureFlagUtil.isPresentAndDisabled(testDB, "QueryStatsCountDistinct"),
       "featureFlagQueryStatsCountDistinct is undefined or enabled");

MongoRunner.stopMongod(conn);
