// Tests that mongos can be given a connection string for the config servers that doesn't exactly
// match the replset config on the config servers, and that it can successfully update it's view
// of the config replset config during startup.

var configRS = new ReplSetTest({name: "configRS", nodes: 1, useHostName: true});
configRS.startSet({configsvr: '', storageEngine: 'wiredTiger'});
var replConfig = configRS.getReplSetConfig();
replConfig.configsvr = true;
configRS.initiate(replConfig);

// Ensure the featureCompatibilityVersion is lastLTSFCV so that the mongos can connect if its
// binary version is lastLTS.
assert.commandWorked(
    configRS.getPrimary().adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

// Build a seed list for the config servers to pass to mongos that uses "localhost" for the
// hostnames even though the replica set config uses the hostname.
var configHosts = [];
for (var i = 0; i < configRS.ports.length; i++) {
    configHosts.push("localhost:" + configRS.ports[i]);
}
var configSeedList = configRS.name + "/" + configHosts.join(",");

var mongos = MongoRunner.runMongos({configdb: configSeedList});

// Do some basic operations to ensure that mongos started up successfully despite the discrepancy
// in the config server replset configuration.
assert.commandWorked(mongos.getDB('admin').runCommand('serverStatus'));

MongoRunner.stopMongos(mongos);
configRS.stopSet();
