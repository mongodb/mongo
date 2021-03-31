/**
 * Tests the resharding feature cannot be used when the feature flag is off.
 *
 * @tags: [
 *   requires_fcv_49,
 * ]
 */
(function() {
"use strict";

load("jstests/sharding/libs/create_sharded_collection_util.js");

const st = new ShardingTest({
    mongos: 1,
    mongosOptions: {setParameter: {featureFlagResharding: false}},
    config: 1,
    configOptions: {setParameter: {featureFlagResharding: false}},
    shards: 1,
    rs: {nodes: 1},
    rsOptions: {setParameter: {featureFlagResharding: false}},
});

const sourceCollection = st.s.getCollection("reshardingDb.coll");

CreateShardedCollectionUtil.shardCollectionWithChunks(
    sourceCollection, {x: 1}, [{min: {x: MinKey}, max: {x: MaxKey}, shard: st.shard0.shardName}]);

assert.commandFailedWithCode(
    st.s.adminCommand({reshardCollection: sourceCollection.getFullName(), key: {y: 1}}),
    ErrorCodes.CommandNotFound);

assert.commandFailedWithCode(
    st.s.adminCommand({abortReshardCollection: sourceCollection.getFullName()}),
    ErrorCodes.CommandNotFound);

const configPrimary = st.configRS.getPrimary();
assert.commandFailedWithCode(configPrimary.adminCommand({
    _configsvrReshardCollection: sourceCollection.getFullName(),
    key: {y: 1},
    writeConcern: {w: 'majority'}
}),
                             ErrorCodes.CommandNotSupported);

assert.commandFailedWithCode(
    configPrimary.adminCommand({_configsvrAbortReshardCollection: sourceCollection.getFullName()}),
    ErrorCodes.CommandNotSupported);

const serverStatusCmd = ({serverStatus: 1, shardingStatistics: 1});
let res = assert.commandWorked(configPrimary.adminCommand(serverStatusCmd));
assert(!res.hasOwnProperty("shardingStatistics"), res.shardingStatistics);

const shardPrimary = st.shard0.rs.getPrimary();
res = assert.commandWorked(shardPrimary.adminCommand(serverStatusCmd));
assert(!res.shardingStatistics.hasOwnProperty("resharding"), res.shardingStatistics);

st.stop();
})();
