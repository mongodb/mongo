/**
 * Test that checks the value of the query stats write command feature flag.
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

// TODO SERVER-109623 this test can be removed when the feature flag is removed.
const conn = MongoRunner.runMongod({
    setParameter: {featureFlagQueryStatsUpdateCommand: false},
});
assert.neq(null, conn, "failed to start mongod");
const testDB = conn.getDB("test");

assert(
    FeatureFlagUtil.isPresentAndDisabled(testDB, "QueryStatsUpdateCommand"),
    "featureFlagQueryStatsUpdateCommand is undefined or enabled",
);

MongoRunner.stopMongod(conn);
