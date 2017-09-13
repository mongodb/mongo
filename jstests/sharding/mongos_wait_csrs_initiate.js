// Tests that mongos will wait for CSRS replica set to initiate.

var configRS = new ReplSetTest({name: "configRS", nodes: 1, useHostName: true});
configRS.startSet({configsvr: '', journal: "", storageEngine: 'wiredTiger'});
var replConfig = configRS.getReplSetConfig();
replConfig.configsvr = true;
var mongos = MongoRunner.runMongos({configdb: configRS.getURL(), waitForConnect: false});

assert.throws(function() {
    new Mongo(mongos.host);
});

jsTestLog("Initiating CSRS");
configRS.initiate(replConfig);

// Ensure the featureCompatibilityVersion is 3.4 so that the mongos can connect if it is version
// 3.4.
assert.commandWorked(configRS.getPrimary().adminCommand({setFeatureCompatibilityVersion: "3.4"}));

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
