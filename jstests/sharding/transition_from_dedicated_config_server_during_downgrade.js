/**
 * Tests that the removeShard command and the transitionFromDedicatedConfigServer command cannot be
 * run during transitional FCV.
 *
 * This test explicitly adds and removes config shard so is not compatible with the
 * config_shard_incompatible suite.
 * @tags: [
 *   requires_fcv_70,
 *   config_shard_incompatible,
 *   featureFlagTransitionToCatalogShard,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/feature_flag_util.js");

{
    const st = new ShardingTest({mongos: 1, shards: 2, configShard: true});

    // Cannot downgrade while there is a config shard.
    assert.commandFailedWithCode(
        st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
        ErrorCodes.CannotDowngrade);

    st.stop();
}

{
    const st = new ShardingTest({shards: 1, config: 3});
    const configRSPrimary = st.configRS.getPrimary();

    // Cannot run 'removeShard' or 'transitionFromDedicatedConfigServer' while in the
    // downgradingToLastLTS FCV.
    configureFailPoint(configRSPrimary, "failDowngrading");
    assert.commandFailedWithCode(
        st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}), 549181);
    assert.commandFailedWithCode(st.s.adminCommand({removeShard: st.shard0.shardName}),
                                 ErrorCodes.IllegalOperation);
    assert.commandFailedWithCode(st.s.adminCommand({transitionFromDedicatedConfigServer: 1}),
                                 7467202);

    st.stop();
}
})();
