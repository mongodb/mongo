// Tests that merizos will wait for CSRS replica set to initiate.

load("jstests/libs/feature_compatibility_version.js");

var configRS = new ReplSetTest({name: "configRS", nodes: 1, useHostName: true});
configRS.startSet({configsvr: '', journal: "", storageEngine: 'wiredTiger'});
var replConfig = configRS.getReplSetConfig();
replConfig.configsvr = true;
var merizos = MongoRunner.runMongos({configdb: configRS.getURL(), waitForConnect: false});

assert.throws(function() {
    new Mongo(merizos.host);
});

jsTestLog("Initiating CSRS");
configRS.initiate(replConfig);

// Ensure the featureCompatibilityVersion is lastStableFCV so that the merizos can connect if it is
// binary version last-stable.
assert.commandWorked(
    configRS.getPrimary().adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));

jsTestLog("getting merizos");
var e;
assert.soon(
    function() {
        try {
            merizos2 = new Mongo(merizos.host);
            return true;
        } catch (ex) {
            e = ex;
            return false;
        }
    },
    function() {
        return "merizos " + merizos.host +
            " did not begin accepting connections in time; final exception: " + tojson(e);
    });

jsTestLog("got merizos");
assert.commandWorked(merizos2.getDB('admin').runCommand('serverStatus'));
configRS.stopSet();
MongoRunner.stopMongos(merizos);