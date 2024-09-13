/*
 * Test auto-bootstrapping a config shard server by:
 * - Initializing a config shard by starting mongod with no parameters
 * - Adding a new replica set node to config shard replica set by specifying --replSet and using
 * replica set reconfig
 * - Adding a new shard to the cluster by specifying --shardsvr and calling addShard on mongos
 * - Adding a new shard by specifying both --replSet and --shardsvr, explicitly initiating the
 * replica set, calling addShard on mongos
 *
 * @tags: [
 *   multiversion_incompatible,
 *   requires_replication,
 *   requires_fcv_80
 * ]
 * */

const node0 = MongoRunner.runMongod({
    setParameter: {
        featureFlagAllMongodsAreSharded: true,
    }
});

// The shard replica set should auto-initiate, and the shard
// should be the config server.
assert.soon(() => node0.adminCommand({hello: 1}).isWritablePrimary);

let config = assert.commandWorked(node0.adminCommand({replSetGetConfig: 1})).config;
assert(config.hasOwnProperty('configsvr') && config.configsvr, tojson(config));

let replSetName = assert.commandWorked(node0.adminCommand({replSetGetStatus: 1})).set;
let url = `${replSetName}/${node0.name}`;
const mongos = MongoRunner.runMongos({configdb: url});

// TODO SERVER-80010: Remove once route role waits for autobootstrap to finish before accepting
// connections.
assert.commandWorked(mongos.adminCommand({transitionFromDedicatedConfigServer: 1}));

// Inserts into replicated collections should succeed.
assert.commandWorked(mongos.getDB('testDB').runCommand({insert: 'testColl', documents: [{x: 1}]}));

// Starting a node with the feature flag and passing --replSet into the startup parameters
// will override replication, and it will not auto-initiate.
const node1 = MongoRunner.runMongod({
    replSet: replSetName,
    setParameter: {
        featureFlagAllMongodsAreSharded: true,
    }
});

assert.commandFailedWithCode(
    node1.getDB('testDB').runCommand({insert: 'testColl', documents: [{x: 1}]}),
    [ErrorCodes.NotWritablePrimary, ErrorCodes.PrimarySteppedDown]);

// A replica set reconfig should succeed in adding the uninitiated node into the auto-initiated
// config shard replica set.
config = assert.commandWorked(node0.adminCommand({replSetGetConfig: 1})).config;
config.members.push({_id: 2, host: node1.host});
config.version++;
assert.commandWorked(node0.adminCommand({replSetReconfig: config}));

assert.soon(() => node1.adminCommand({hello: 1}).secondary);

// Specifying --shardsvr should override auto-initialization of sharding components.
// Instead, this node should await an external addShard from mongos.
const node2 = MongoRunner.runMongod({
    shardsvr: "",
    setParameter: {
        featureFlagAllMongodsAreSharded: true,
    }
});

// Replication was not overridden, so this node should still auto-initiate replication.
assert.soon(() => node2.adminCommand({hello: 1}).isWritablePrimary);
// Since we passed in --shardsvr, this node will not default to being the config shard.
config = assert.commandWorked(node2.adminCommand({replSetGetConfig: 1})).config;
assert(!config.hasOwnProperty('configsvr'), tojson(config));

replSetName = assert.commandWorked(node2.adminCommand({replSetGetStatus: 1})).set;
url = `${replSetName}/${node2.name}`;
assert.commandWorked(mongos.adminCommand({addShard: url}));

// Specifying both --replSet and --shardSvr will override both replication and sharding. The node
// will await an external replSetInitiate or heartbeat from another node to initiate replication,
// and it will await an external addShard command.
const node3 = MongoRunner.runMongod({
    replSet: "rs",
    shardsvr: "",
    setParameter: {
        featureFlagAllMongodsAreSharded: true,
    }
});
assert.commandFailedWithCode(
    node3.getDB('testDB').runCommand({insert: 'testColl', documents: [{x: 1}]}),
    [ErrorCodes.NotWritablePrimary, ErrorCodes.PrimarySteppedDown]);

// An external replSetInitiate should initiate the replica set successfully.
assert.commandWorked(node3.adminCommand({replSetInitiate: 1}));
assert.soon(() => node3.adminCommand({hello: 1}).isWritablePrimary);

replSetName = assert.commandWorked(node3.adminCommand({replSetGetStatus: 1})).set;
url = `${replSetName}/${node3.name}`;
assert.commandWorked(mongos.adminCommand({addShard: url}));

const numShards = assert.commandWorked(mongos.getDB('config').runCommand({count: 'shards'})).n;
assert.eq(numShards, 3);

MongoRunner.stopMongod(node0);
MongoRunner.stopMongod(node1);
MongoRunner.stopMongod(node2);
MongoRunner.stopMongod(node3);
MongoRunner.stopMongos(mongos);
