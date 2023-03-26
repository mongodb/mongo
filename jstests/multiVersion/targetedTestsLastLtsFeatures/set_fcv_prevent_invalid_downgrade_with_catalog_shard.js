/**
 * Test to ensure that：
 *      1. The FCV cannot be downgraded to a version that does not have catalog shards if catalog
 *         shard is enabled.
 *      2. If the FCV does get downgraded to a version that does not support catalog shards, a
 *         catalog shard cannot be created (this can occur if an FCV downgrade happens concurrently
 *         with the creation of a catalog shard).
 *
 * @tags: [featureFlagCatalogShard, featureFlagTransitionToCatalogShard]
 */
(function() {
"use strict";

/* Downgrading FCV to an unsupported version when catalogShard is enabled. */
jsTest.log("Downgrading FCV to an unsupported version when catalogShard is enabled.");
var st = new ShardingTest({catalogShard: true});
var mongosAdminDB = st.s.getDB("admin");

let errRes = assert.commandFailedWithCode(
    mongosAdminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV}),
    ErrorCodes.CannotDowngrade);
assert.eq(errRes.errmsg,
          `Cannot downgrade featureCompatibilityVersion to ${lastLTSFCV} with a catalog shard as it is not supported in earlier versions. Please transition the config server to dedicated mode using the transitionToDedicatedConfigServer command.`);

errRes = assert.commandFailedWithCode(
    mongosAdminDB.runCommand({setFeatureCompatibilityVersion: lastContinuousFCV}),
    ErrorCodes.CannotDowngrade);
assert.eq(errRes.errmsg,
          `Cannot downgrade featureCompatibilityVersion to ${lastContinuousFCV} with a catalog shard as it is not supported in earlier versions. Please transition the config server to dedicated mode using the transitionToDedicatedConfigServer command.`);

var res = st.config0.getDB("admin").runCommand({getParameter: 1, featureCompatibilityVersion: 1});
assert(res.featureCompatibilityVersion);
assert.eq(res.featureCompatibilityVersion.version, latestFCV);

st.stop();

/* Attempting to create a catalogShard on an unsupported FCV. */
jsTest.log("Attempting to create a catalogShard on an unsupported FCV.");
st = new ShardingTest({catalogShard: false});
mongosAdminDB = st.s.getDB("admin");

assert.commandWorked(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
assert.commandFailedWithCode(mongosAdminDB.runCommand({transitionToCatalogShard: 1}), 7467202);

st.stop();
})();
