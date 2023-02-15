/**
 * Test to ensure that：
 *      1. The FCV cannot be downgraded to a version that does not have catalog shards if catalog
 *         shard is enabled.
 *      2. If the FCV does get downgraded to a version that does not support catalog shards, a
 *         catalog shard cannot be created (this can occur if an FCV downgrade happens concurrently
 *         with the creation of a catalog shard).
 *
 * @tags: [featureFlagCatalogShard]
 */
(function() {
"use strict";

/* Downgrading FCV to an unsupported version when catalogShard is enabled. */
jsTest.log("Downgrading FCV to an unsupported version when catalogShard is enabled.");
var st = new ShardingTest({catalogShard: true});
var mongosAdminDB = st.s.getDB("admin");

assert.commandFailed(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV}),
                     `cannot downgrade featureCompatibilityVersion to ${
                         lastLTSFCV} when catalog shard is enabled as it may result in data loss`);

var res = st.config0.getDB("admin").runCommand({getParameter: 1, featureCompatibilityVersion: 1});
assert(res.featureCompatibilityVersion);
assert.eq(res.featureCompatibilityVersion.version, latestFCV);

st.stop();

/* Attempting to create a catalogShard on an unsupported FCV. */
jsTest.log("Attempting to create a catalogShard on an unsupported FCV.");
const kCatalogShardId = "catalogShard";
st = new ShardingTest({catalogShard: false});
mongosAdminDB = st.s.getDB("admin");

assert.commandWorked(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
assert.commandFailed(
    mongosAdminDB.runCommand({addShard: st.configRS.getURL(), name: kCatalogShardId}),
    "Cannot add a shard with catalogShard as its name if the catalog shard feature flag is not enabled");

st.stop();
})();
