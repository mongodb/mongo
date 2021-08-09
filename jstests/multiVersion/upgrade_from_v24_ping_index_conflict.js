/**
 * In v2.4, an index {ping: 1} named "ping_" was created on the config.lockpings collection.
 * Starting in 4.2, creating an index identical to an existing index, but with a different name, is
 * explicitly disallowed.
 *
 * Internally, modern mongods implicitly create an index on {ping: 1} named "ping_1" when stepping
 * up to primary - conflicting with index naming restrictions that there cannot be 2 identical
 * indexes with different names.
 *
 * Tests that upgrading a config server to v4.2 fails to start up and relays a message to drop the
 * "ping_" index introduced in v2.4 before upgrade.
 */
(function() {
"use strict";

const cluster = new ShardingTest({
    shards: 2,
    other: {
        mongosOptions: {binVersion: "last-stable"},
        configOptions: {
            binVersion: "last-stable",
        },
        rsOptions: {binVersion: "last-stable"},
    },
    rs: {nodes: 3}
});
const configRS = cluster.configRS;

// Returns the 'config' database from the cluster's current primary config server.
const getConfigDB = function() {
    return configRS.getPrimary().getDB("config");
};

// Asserts that an index with indexName exists in on the config.lockpings collection.
const assertLockpingsHasIndex = function(indexName, configDB) {
    const listIndexesResult = assert.commandWorked(configDB.runCommand({listIndexes: "lockpings"}));

    const containsIndex = listIndexesResult.cursor.firstBatch.reduce((acc, indexSpec) => {
        return acc || indexSpec.name == indexName;
    }, false);

    assert.eq(true,
              containsIndex,
              `Index with name ${indexName} does not exist: ${tojson(listIndexesResult)}`);
};

jsTest.log("Setting up collection to contain index 'ping_'");
// Drop the implicitly created "ping_1" index to allow for the creation of the "ping_"
// index.
let configDB = getConfigDB();
assert.commandWorked(configDB["lockpings"].dropIndex({ping: 1}));
assert.commandWorked(
    configDB.runCommand({createIndexes: "lockpings", indexes: [{key: {ping: 1}, name: "ping_"}]}));
assertLockpingsHasIndex("ping_", configDB);

jsTest.log("Testing secondary crashes on upgrade to 'latest' due to 'ping_' index");
const secondary = configRS.getSecondary();
try {
    configRS.restart(secondary, {binVersion: "latest"});
} catch (error) {
    // Catch the error thrown when the secondary crashes during restart.
}
assert.soon(() => {
    // Confirm the secondary crashed with the fatal assertion with instructions to drop the "ping_"
    // index.
    return rawMongoProgramOutput().search(/Fatal assertion 5272800/) != -1;
});

jsTest.log("Restarting secondary as 'last-stable' to drop 'ping_' index");
// Before upgrading, user must drop the "ping_" index and await replication.
configRS.restart(secondary, {binVersion: "last-stable", allowedExitCode: MongoRunner.EXIT_ABORT});
assert.commandWorked(configDB["lockpings"].dropIndex("ping_"));
configRS.awaitReplication();

jsTest.log("Testing rolling upgrade succeeds in absence of 'ping_' index");
// First upgrade both secondaries.
const secondaries = configRS.getSecondaries();
configRS.restart(secondaries[0], {binVersion: "latest"});
configRS.restart(secondaries[1], {binVersion: "latest"});

// Step up a new primary before upgrading the existing primary.
const originalPrimary = configRS.getPrimary();
configRS.getSecondary().adminCommand({replSetStepUp: 1});
configRS.restart(originalPrimary, {binVersion: "latest"});

configRS.awaitNodesAgreeOnPrimary();

// The "ping_1" index should exist once an upgraded (v4.2) config server steps up to primary.
assertLockpingsHasIndex("ping_1", getConfigDB());

cluster.stop();
})();
