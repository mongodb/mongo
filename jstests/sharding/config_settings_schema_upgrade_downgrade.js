/**
 * TODO (SERVER-70763) remove this test after 7.0 becomes lastLTS
 *
 * Tests that a schema is added to the config.settings collection on upgrade and removed on
 * downgrade.
 *
 * @tags: [multiversion_incompatible, featureFlagConfigSettingsSchema, does_not_support_stepdowns]
 */
(function() {
'use strict';

load("jstests/libs/feature_flag_util.js");

var st = new ShardingTest({shards: 1, config: 2});

// Validator should be created for new clusters in 6.2
let validatorDoc = st.config.getCollectionInfos({name: "settings"})[0].options.validator;
assert(validatorDoc);

// Validator should be removed on downgrade
st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV});
validatorDoc = st.config.getCollectionInfos({name: "settings"})[0].options.validator;
assert(!validatorDoc);

// Validator should be added in on upgrade
st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV});
validatorDoc = st.config.getCollectionInfos({name: "settings"})[0].options.validator;
assert(validatorDoc);

st.stop();
})();
