/**
 * Test that checks the value of the query stats insert command feature flag.
 *
 * @tags: [featureFlagQueryStatsInsert]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

// TODO SERVER-109441 this test can be removed when the feature flag is removed.
const conn = MongoRunner.runMongod({
    setParameter: {featureFlagQueryStatsInsert: false},
});
assert.neq(null, conn, "failed to start mongod");
const testDB = conn.getDB("test");

assert(
    FeatureFlagUtil.isPresentAndDisabled(testDB, "QueryStatsInsert"),
    "featureFlagQueryStatsInsert is undefined or enabled",
);

MongoRunner.stopMongod(conn);
