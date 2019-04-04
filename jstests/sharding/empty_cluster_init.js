//
// Tests initialization of an empty cluster with multiple merizoses.
// Starts a bunch of merizoses in parallel, and ensures that there's only a single config
// version initialization.
//

var configRS = new ReplSetTest({name: "configRS", nodes: 3, useHostName: true});
configRS.startSet({configsvr: '', journal: "", storageEngine: 'wiredTiger'});
var replConfig = configRS.getReplSetConfig();
replConfig.configsvr = true;
configRS.initiate(replConfig);

//
// Start a bunch of merizoses which will probably interfere
//

jsTest.log("Starting first set of merizoses in parallel...");

var merizoses = [];
for (var i = 0; i < 3; i++) {
    var merizos = MerizoRunner.runMerizos(
        {binVersion: "latest", configdb: configRS.getURL(), waitForConnect: false});
    merizoses.push(merizos);
}

// Eventually connect to a merizo host, to be sure that the config upgrade happened
// (This can take longer on extremely slow bbots or VMs)
var merizosConn = null;
assert.soon(function() {
    try {
        merizosConn = new Merizo(merizoses[0].host);
        return true;
    } catch (e) {
        print("Waiting for connect...");
        printjson(e);
        return false;
    }
}, "Merizos " + merizoses[0].host + " did not start.", 5 * 60 * 1000);

var version = merizosConn.getCollection("config.version").findOne();

//
// Start a second set of merizoses which should respect the initialized version
//

jsTest.log("Starting second set of merizoses...");

for (var i = 0; i < 3; i++) {
    var merizos = MerizoRunner.runMerizos(
        {binVersion: "latest", configdb: configRS.getURL(), waitForConnect: false});
    merizoses.push(merizos);
}

var connectToMerizos = function(host) {
    // Eventually connect to a host
    assert.soon(function() {
        try {
            merizosConn = new Merizo(host);
            return true;
        } catch (e) {
            print("Waiting for connect to " + host);
            printjson(e);
            return false;
        }
    }, "merizos " + host + " did not start.", 5 * 60 * 1000);
};

for (var i = 0; i < merizoses.length; i++) {
    connectToMerizos(merizoses[i].host);
}

// Shut down our merizoses now that we've tested them
for (var i = 0; i < merizoses.length; i++) {
    MerizoRunner.stopMerizos(merizoses[i]);
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
