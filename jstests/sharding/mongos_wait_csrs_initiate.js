// Tests that mongos will wait for CSRS replica set to initiate.
// @tags: [multiversion_incompatible]

var configRS = new ReplSetTest({name: "configRS", nodes: 1, useHostName: true});
configRS.startSet({configsvr: '', storageEngine: 'wiredTiger'});
var replConfig = configRS.getReplSetConfig();
replConfig.configsvr = true;
var mongos = MongoRunner.runMongos({configdb: configRS.getURL(), waitForConnect: false});

assert.throws(function() {
    new Mongo(mongos.host);
});

jsTestLog("Initiating CSRS");
configRS.initiate(replConfig);

// Ensure the featureCompatibilityVersion is lastLTSFCV so that the mongos can connect if it is
// binary version last-lts.
assert.commandWorked(
    configRS.getPrimary().adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

jsTestLog("getting mongos");
var e;
assert.soon(
    function() {
        try {
            mongos2 = new Mongo(mongos.host);
            return true;
        } catch (ex) {
            e = ex;
            return false;
        }
    },
    function() {
        return "mongos " + mongos.host +
            " did not begin accepting connections in time; final exception: " + tojson(e);
    });

jsTestLog("got mongos");
assert.commandWorked(mongos2.getDB('admin').runCommand('serverStatus'));
configRS.stopSet();
MongoRunner.stopMongos(mongos);
