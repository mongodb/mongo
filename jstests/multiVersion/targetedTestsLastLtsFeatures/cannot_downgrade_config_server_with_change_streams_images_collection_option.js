/**
 * Verifies a config server cannot downgrade with a collection with changeStreamPreAndPostImages
 * enabled.
 *
 * @tags: [featureFlagCatalogShard, featureFlagTransitionToCatalogShard]
 */
(function() {
"use strict";

const st = new ShardingTest({config: 1, shards: 1});

// A collection on a shard with changeStreamPreAndPostImages shouldn't impact downgrade.
const validShardNS = "foo.bar";
assert.commandWorked(st.s.getCollection(validShardNS).insert({x: 1}));
assert.commandWorked(
    st.s.getDB("foo").runCommand({collMod: "bar", changeStreamPreAndPostImages: {enabled: true}}));

// A collection on the config server with changeStreamPreAndPostImages should prevent downgrade. The
// config server can only downgrade when in dedicated mode and in this mode the only user
// accessible collections on it are in the config and admin databases, which never allow this
// option, so we have to create a collection on a separate db via direct connection.
const directConfigNS = "directDB.onConfig";
assert.commandWorked(st.configRS.getPrimary().getCollection(directConfigNS).insert({x: 1}));
assert.commandWorked(st.configRS.getPrimary().getDB("directDB").runCommand({
    collMod: "onConfig",
    changeStreamPreAndPostImages: {enabled: true}
}));

assert.commandFailedWithCode(st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}),
                             ErrorCodes.CannotDowngrade);

// Unset the option on the config server collection and now the config server can downgrade.
assert.commandWorked(st.configRS.getPrimary().getDB("directDB").runCommand({
    collMod: "onConfig",
    changeStreamPreAndPostImages: {enabled: false}
}));

assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

st.stop();
})();
