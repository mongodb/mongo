// Tests that merizos can be given a connection string for the config servers that doesn't exactly
// match the replset config on the config servers, and that it can successfully update it's view
// of the config replset config during startup.

load("jstests/libs/feature_compatibility_version.js");
var configRS = new ReplSetTest({name: "configRS", nodes: 1, useHostName: true});
configRS.startSet({configsvr: '', journal: "", storageEngine: 'wiredTiger'});
var replConfig = configRS.getReplSetConfig();
replConfig.configsvr = true;
configRS.initiate(replConfig);

// Ensure the featureCompatibilityVersion is lastStableFCV so that the merizos can connect if its
// binary version is lastStable.
assert.commandWorked(
    configRS.getPrimary().adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));

// Build a seed list for the config servers to pass to merizos that uses "localhost" for the
// hostnames even though the replica set config uses the hostname.
var configHosts = [];
for (var i = 0; i < configRS.ports.length; i++) {
    configHosts.push("localhost:" + configRS.ports[i]);
}
var configSeedList = configRS.name + "/" + configHosts.join(",");

var merizos = MongoRunner.runMongos({configdb: configSeedList});

// Do some basic operations to ensure that merizos started up successfully despite the discrepancy
// in the config server replset configuration.
assert.commandWorked(merizos.getDB('admin').runCommand('serverStatus'));

MongoRunner.stopMongos(merizos);
configRS.stopSet();
