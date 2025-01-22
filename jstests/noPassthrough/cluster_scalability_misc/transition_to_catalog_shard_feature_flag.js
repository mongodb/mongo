/**
 * Verifies the transitionFromDedicatedConfigServer feature flag guards running the config shard
 * transition commands.
 *
 * @tags: [requires_fcv_80]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

delete TestData.setParameters.featureFlagTransitionToCatalogShard;
delete TestData.setParametersMongos.featureFlagTransitionToCatalogShard;

const st = new ShardingTest({
    mongos: 1,
    config: 1,
    shards: 1,
    rs: {nodes: 1},
});
st.stopAllConfigServers({}, true);
st.restartAllConfigServers(
    {configsvr: "", setParameter: {featureFlagTransitionToCatalogShard: false}});

st.restartMongos(0, {setParameter: {featureFlagTransitionToCatalogShard: false}, restart: true});

// None of the transition commands can be run on mongos or the config server.
assert.commandFailedWithCode(st.s.adminCommand({transitionFromDedicatedConfigServer: 1}), 8454804);
assert.commandFailedWithCode(st.s.adminCommand({transitionToDedicatedConfigServer: 1}), 7368401);

const configPrimary = st.configRS.getPrimary();
assert.commandFailedWithCode(
    configPrimary.adminCommand({_configsvrTransitionFromDedicatedConfigServer: 1}), 8454803);
assert.commandFailedWithCode(
    configPrimary.adminCommand({_configsvrTransitionToDedicatedConfigServer: 1}), 7368402);

st.stop();
