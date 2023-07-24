/*
 * Test auto-initiating a replica set.
 *
 * @tags: [
 *   multiversion_incompatible,
 *   requires_replication,
 * ]
 * */

(function() {
"use strict";
// Starting a mongod with 'featureFlagAllMongodsAreSharded' should auto-initiate a replica set
// without needing an explicit replica set initiate command.
let node = MongoRunner.runMongod({
    setParameter: {
        featureFlagAllMongodsAreSharded: true,
    }
});

assert.soon(() => node.adminCommand({hello: 1}).isWritablePrimary);

// Inserts into replicated collections should succeed.
assert.commandWorked(node.getDB('testDB').runCommand({insert: 'testColl', documents: [{x: 1}]}));
MongoRunner.stopMongod(node);

// Starting a node with the feature flag and passing in --replSet into the startup parameters
// will startup the node as a replica set node. It will not auto-initiate.
node = MongoRunner.runMongod({
    replSet: 'rs',
    setParameter: {
        featureFlagAllMongodsAreSharded: true,
    }
});

assert.commandFailedWithCode(
    node.getDB('testDB').runCommand({insert: 'testColl', documents: [{x: 1}]}),
    ErrorCodes.NotWritablePrimary);

// An external replSetInitiate should initiate the replica set successfully.
assert.commandWorked(node.adminCommand({replSetInitiate: 1}));

assert.soon(() => node.adminCommand({hello: 1}).isWritablePrimary);

assert.commandWorked(node.getDB('testDB').runCommand({insert: 'testColl', documents: [{x: 1}]}));

MongoRunner.stopMongod(node);

// A replica set reconfig should succeed in adding an uninitiated node into an auto-initiated
// replica set.
const node0 = MongoRunner.runMongod({
    setParameter: {
        featureFlagAllMongodsAreSharded: true,
    }
});

assert.soon(() => node0.adminCommand({hello: 1}).isWritablePrimary);

assert.commandWorked(node0.getDB('testDB').runCommand({insert: 'testColl', documents: [{x: 1}]}));

const node1 = MongoRunner.runMongod({
    replSet: 'rs',
    setParameter: {
        featureFlagAllMongodsAreSharded: true,
    }
});

const config = node0.getDB("local").system.replset.findOne();
config.members.push({_id: 2, host: node1.host});
config.version++;
assert.commandWorked(node0.adminCommand({replSetReconfig: config}));

assert.soon(() => node1.adminCommand({hello: 1}).secondary);

MongoRunner.stopMongod(node0);
MongoRunner.stopMongod(node1);
}());
