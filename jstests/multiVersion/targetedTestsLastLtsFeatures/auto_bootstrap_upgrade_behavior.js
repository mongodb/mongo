import "jstests/multiVersion/libs/multi_rs.js";

import {ReplSetTest} from "jstests/libs/replsettest.js";

function fetchShardIdentityDoc(conn) {
    return conn.getDB('admin').system.version.findOne({_id: 'shardIdentity'});
}

(function() {
jsTest.log("Test auto-bootstrap doesn't happen for standalone in old fcv");

let oldStandalone = MongoRunner.runMongod({binVersion: 'last-lts'});

MongoRunner.stopMongod(oldStandalone, null, {noCleanData: true});

let newStandalone = MongoRunner.runMongod({
    noCleanData: true,
    dbpath: oldStandalone.dbpath,
    port: oldStandalone.port,
    setParameter: {featureFlagAllMongodsAreSharded: true}
});

assert.soon(() => {
    return newStandalone.getDB('admin').hello().isWritablePrimary;
});

assert.eq(null, fetchShardIdentityDoc(newStandalone));

MongoRunner.stopMongod(newStandalone, null);
})();

(function() {
jsTest.log("Test auto-bootstrap doesn't happen for replSet in old fcv");

let nodeOption = {binVersion: 'last-lts'};
// Need at least 2 nodes because upgradeSet method needs to be able call step down
// with another primary eligible node available.
let replSet = new ReplSetTest({nodes: [nodeOption, nodeOption]});
replSet.startSet();
replSet.initiate();

replSet.upgradeSet({binVersion: 'latest', setParameter: {featureFlagAllMongodsAreSharded: true}});

let primary = replSet.getPrimary();
assert.eq(null, fetchShardIdentityDoc(primary));

assert.commandWorked(
    primary.adminCommand({transitionToShardedCluster: 1, writeConcern: {w: 'majority'}}));

assert.neq(null, fetchShardIdentityDoc(primary));
assert.neq(null, primary.getDB('config').shards.findOne());

replSet.stopSet();
})();

(function() {
jsTest.log("Test auto-bootstrap fails with upgradeBackCompat and downgradeBackCompat");

let nodeOption = {binVersion: 'last-lts'};
// Need at least 2 nodes because upgradeSet method needs to be able call step down
// with another primary eligible node available.
let replSet = new ReplSetTest({nodes: [nodeOption, nodeOption]});
replSet.startSet();
replSet.initiate();

replSet.upgradeSet({
    binVersion: 'latest',
    upgradeBackCompat: '',
    setParameter: {featureFlagAllMongodsAreSharded: true}
});

let primary = replSet.getPrimary();
assert.eq(null, fetchShardIdentityDoc(primary));

assert.commandFailedWithCode(
    primary.adminCommand({transitionToShardedCluster: 1, writeConcern: {w: 'majority'}}), 8263000);

// No transformation took place.
assert.eq(null, fetchShardIdentityDoc(primary));
assert.eq(null, primary.getDB('config').shards.findOne());

// Repeat with downgradeBackCompat

replSet.upgradeSet({
    binVersion: 'latest',
    downgradeBackCompat: '',
    removeOptions: ['upgradeBackCompat'],
    setParameter: {featureFlagAllMongodsAreSharded: true}
});

primary = replSet.getPrimary();
assert.eq(null, fetchShardIdentityDoc(primary));

assert.commandFailedWithCode(
    primary.adminCommand({transitionToShardedCluster: 1, writeConcern: {w: 'majority'}}), 8263000);

// No transformation took place.
assert.eq(null, fetchShardIdentityDoc(primary));
assert.eq(null, primary.getDB('config').shards.findOne());

replSet.stopSet();
})();
