/*
 * Tests that ShardingReady correctly gets set for various server types on startup and restart.
 *
 * TODO SERVER-80010: Either modify this test file to test that the router role correctly waits for
 * auto-bootstrap to finish before accepting connections or remove it if another test file is
 * written.
 *
 * @tags: [
 *   multiversion_incompatible,
 *   requires_replication,
 *   does_not_support_stepdowns,
 *   requires_persistence
 * ]
 * */
function isShardingReady(conn) {
    const res = assert.commandWorked(conn.adminCommand({getShardingReady: 1}));
    return res.isReady;
}

// Ensure that the config shard primary eventually has ShardingReady set.
let configPrimary = MongoRunner.runMongod({
    setParameter: {
        featureFlagAllMongodsAreSharded: true,
    },
});
assert.soon(() => isShardingReady(configPrimary));

// Startup a mongos.
let configSvrReplSetName;
assert.soon(() => {
    let result = configPrimary.adminCommand({replSetGetStatus: 1});
    if (result.ok !== 1) {
        return false;
    }
    configSvrReplSetName = result.set;
    return true;
});
let url = `${configSvrReplSetName}/${configPrimary.name}`;
const mongos = MongoRunner.runMongos({configdb: url});

// Restart the config shard primary and ensure that it still has ShardingReady set.
MongoRunner.stopMongod(configPrimary);
configPrimary = MongoRunner.runMongod({restart: configPrimary});
assert.soon(() => isShardingReady(configPrimary));

// Add a config shard secondary and ensure that it eventually has ShardingReady set.
let configSecondary = MongoRunner.runMongod({
    replSet: configSvrReplSetName,
    setParameter: {
        featureFlagAllMongodsAreSharded: true,
    },
});
assert(!isShardingReady(configSecondary));
let configSvrReplSetConfig;
assert.soon(() => {
    let result = configPrimary.adminCommand({replSetGetConfig: 1});
    if (result.ok !== 1) {
        return false;
    }
    configSvrReplSetConfig = result.config;
    return true;
});
configSvrReplSetConfig.members.push({_id: 2, host: configSecondary.host});
configSvrReplSetConfig.version++;
assert.commandWorked(configPrimary.adminCommand({replSetReconfig: configSvrReplSetConfig}));
assert.soon(() => isShardingReady(configSecondary));

// Restart the config shard secondary and ensure that it eventually has ShardingReady set.
MongoRunner.stopMongod(configSecondary);
configSecondary = MongoRunner.runMongod({restart: configSecondary});
assert.soon(() => isShardingReady(configSecondary));

// Add a shard server primary and ensure that it immediately has ShardingReady set on startup.
let shardPrimary = MongoRunner.runMongod({
    shardsvr: "",
    setParameter: {
        featureFlagAllMongodsAreSharded: true,
    },
});
assert(isShardingReady(shardPrimary));
let shardSvrReplSetName;
assert.soon(() => {
    let result = shardPrimary.adminCommand({replSetGetStatus: 1});
    if (result.ok !== 1) {
        return false;
    }
    shardSvrReplSetName = result.set;
    return true;
});
url = `${shardSvrReplSetName}/${shardPrimary.name}`;
assert.commandWorked(mongos.adminCommand({addShard: url}));

// Restart the shard server primary and ensure that it still has ShardingReady set.
MongoRunner.stopMongod(shardPrimary);
shardPrimary = MongoRunner.runMongod({restart: shardPrimary});
assert(isShardingReady(shardPrimary));

// Add a shard server secondary and ensure that it immediately has ShardingReady set on startup.
let shardSecondary = MongoRunner.runMongod({
    shardsvr: "",
    replSet: shardSvrReplSetName,
    setParameter: {
        featureFlagAllMongodsAreSharded: true,
    },
});
assert(isShardingReady(shardSecondary));
let shardSvrReplSetConfig;
assert.soon(() => {
    let result = shardPrimary.adminCommand({replSetGetConfig: 1});
    if (result.ok !== 1) {
        return false;
    }
    shardSvrReplSetConfig = result.config;
    return true;
});
shardSvrReplSetConfig.members.push({_id: 2, host: shardSecondary.host});
shardSvrReplSetConfig.version++;
assert.commandWorked(shardPrimary.adminCommand({replSetReconfig: shardSvrReplSetConfig}));

// Restart the shard server secondary and ensure that it still has ShardingReady set.
MongoRunner.stopMongod(shardSecondary);
shardSecondary = MongoRunner.runMongod({restart: shardSecondary});
assert.soon(() => isShardingReady(shardSecondary));

if (TestData.configShard) {
    // Transition the config shard to a dedicated config server and ensure that ShardingReady is
    // still set on the config server primary and secondary.
    assert.commandWorked(mongos.adminCommand({transitionToDedicatedConfigServer: 1}));
    assert.soon(() => isShardingReady(configPrimary));
    assert.soon(() => isShardingReady(configSecondary));
}

// Restart the dedicated config server primary and secondaries and ensure that they still have
// ShardingReady set.
MongoRunner.stopMongod(configPrimary);
configPrimary = MongoRunner.runMongod({restart: configPrimary});
MongoRunner.stopMongod(configSecondary);
configSecondary = MongoRunner.runMongod({restart: configSecondary});
assert.soon(() => isShardingReady(configPrimary));
assert.soon(() => isShardingReady(configSecondary));

MongoRunner.stopMongos(mongos);
MongoRunner.stopMongod(shardSecondary);
MongoRunner.stopMongod(shardPrimary);
MongoRunner.stopMongod(configSecondary);
MongoRunner.stopMongod(configPrimary);
