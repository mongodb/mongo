/**
 * Verifies the transitionFromDedicatedConfigServer feature flag guards running the config shard
 * transition commands.
 *
 * @tags: [requires_fcv_70]
 */
(function() {
"use strict";

delete TestData.setParameters.featureFlagTransitionToCatalogShard;
delete TestData.setParametersMongos.featureFlagTransitionToCatalogShard;

const st = new ShardingTest({
    mongos: 1,
    mongosOptions: {setParameter: {featureFlagTransitionToCatalogShard: false}},
    config: 1,
    configOptions: {setParameter: {featureFlagTransitionToCatalogShard: false}},
    shards: 1,
    rs: {nodes: 1},
    rsOptions: {setParameter: {featureFlagTransitionToCatalogShard: false}},
});

// None of the transition commands can be run on mongos or the config server.
assert.commandFailedWithCode(st.s.adminCommand({transitionFromDedicatedConfigServer: 1}),
                             ErrorCodes.CommandNotFound);
assert.commandFailedWithCode(st.s.adminCommand({transitionToDedicatedConfigServer: 1}), 7368401);

const configPrimary = st.configRS.getPrimary();
assert.commandFailedWithCode(
    configPrimary.adminCommand({_configsvrTransitionFromDedicatedConfigServer: 1}),
    ErrorCodes.CommandNotFound);
assert.commandFailedWithCode(
    configPrimary.adminCommand({_configsvrTransitionToDedicatedConfigServer: 1}), 7368402);

st.stop();
}());
