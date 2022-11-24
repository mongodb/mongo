/**
 * Tests that the serverStatus output does not contain a globalIndex section when the feature flag
 * is off.
 * @tags: [
 * requires_fcv_61, featureFlagGlobalIndexes, multiversion_incompatible
 * ]
 */
(function() {
"use strict";

delete TestData.setParameters.featureFlagGlobalIndexes;
delete TestData.setParametersMongos.featureFlagGlobalIndexes;

// Default value of the featureFlagGlobalIndexes is false
const st = new ShardingTest({
    mongos: 1,
    mongosOptions: {setParameter: {featureFlagGlobalIndexes: false}},
    config: 1,
    configOptions: {setParameter: {featureFlagGlobalIndexes: false}},
    shards: 1,
    rs: {nodes: 1},
    rsOptions: {setParameter: {featureFlagGlobalIndexes: false}},
});
const configPrimary = st.configRS.getPrimary();
const serverStatusCmd = ({serverStatus: 1, shardingStatistics: 1});
let res = assert.commandWorked(configPrimary.adminCommand(serverStatusCmd));
assert(!res.shardingStatistics.hasOwnProperty("globalIndex"), res.shardingStatistics);

const shardPrimary = st.shard0.rs.getPrimary();
res = assert.commandWorked(shardPrimary.adminCommand(serverStatusCmd));
assert(!res.shardingStatistics.hasOwnProperty("globalIndex"), res.shardingStatistics);

st.stop();
})();
