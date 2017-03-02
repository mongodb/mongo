//
// Tests initialization of an empty cluster with multiple bongoses.
// Starts a bunch of bongoses in parallel, and ensures that there's only a single config
// version initialization.
//

var configRS = new ReplSetTest({name: "configRS", nodes: 3, useHostName: true});
configRS.startSet({configsvr: '', journal: "", storageEngine: 'wiredTiger'});
var replConfig = configRS.getReplSetConfig();
replConfig.configsvr = true;
configRS.initiate(replConfig);

//
// Start a bunch of bongoses which will probably interfere
//

jsTest.log("Starting first set of bongoses in parallel...");

var bongoses = [];
for (var i = 0; i < 3; i++) {
    var bongos = BongoRunner.runBongos(
        {binVersion: "latest", configdb: configRS.getURL(), waitForConnect: false});
    bongoses.push(bongos);
}

// Eventually connect to a bongo host, to be sure that the config upgrade happened
// (This can take longer on extremely slow bbots or VMs)
var bongosConn = null;
assert.soon(function() {
    try {
        bongosConn = new Bongo(bongoses[0].host);
        return true;
    } catch (e) {
        print("Waiting for connect...");
        printjson(e);
        return false;
    }
}, "Bongos " + bongoses[0].host + " did not start.", 5 * 60 * 1000);

var version = bongosConn.getCollection("config.version").findOne();

//
// Start a second set of bongoses which should respect the initialized version
//

jsTest.log("Starting second set of bongoses...");

for (var i = 0; i < 3; i++) {
    var bongos = BongoRunner.runBongos(
        {binVersion: "latest", configdb: configRS.getURL(), waitForConnect: false});
    bongoses.push(bongos);
}

var connectToBongos = function(host) {
    // Eventually connect to a host
    assert.soon(function() {
        try {
            bongosConn = new Bongo(host);
            return true;
        } catch (e) {
            print("Waiting for connect to " + host);
            printjson(e);
            return false;
        }
    }, "bongos " + host + " did not start.", 5 * 60 * 1000);
};

for (var i = 0; i < bongoses.length; i++) {
    connectToBongos(bongoses[i].host);
}

// Shut down our bongoses now that we've tested them
for (var i = 0; i < bongoses.length; i++) {
    BongoRunner.stopBongos(bongoses[i]);
}

//
// Check version and that the version was only updated once
//

assert.eq(5, version.minCompatibleVersion);
assert.eq(6, version.currentVersion);
assert(version.clusterId);
assert.eq(undefined, version.excluding);

var oplog = configRS.getPrimary().getDB('local').oplog.rs;
var updates = oplog.find({ns: "config.version"}).toArray();
assert.eq(1, updates.length, 'ops to config.version: ' + tojson(updates));

configRS.stopSet(15);
