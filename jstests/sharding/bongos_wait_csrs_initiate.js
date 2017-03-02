// Tests that bongos will wait for CSRS replica set to initiate.

var configRS = new ReplSetTest({name: "configRS", nodes: 1, useHostName: true});
configRS.startSet({configsvr: '', journal: "", storageEngine: 'wiredTiger'});
var replConfig = configRS.getReplSetConfig();
replConfig.configsvr = true;
var bongos = BongoRunner.runBongos({configdb: configRS.getURL(), waitForConnect: false});

assert.throws(function() {
    new Bongo(bongos.host);
});

jsTestLog("Initiating CSRS");
configRS.initiate(replConfig);

jsTestLog("getting bongos");
var e;
assert.soon(
    function() {
        try {
            bongos2 = new Bongo(bongos.host);
            return true;
        } catch (ex) {
            e = ex;
            return false;
        }
    },
    function() {
        return "bongos " + bongos.host +
            " did not begin accepting connections in time; final exception: " + tojson(e);
    });

jsTestLog("got bongos");
assert.commandWorked(bongos2.getDB('admin').runCommand('serverStatus'));
