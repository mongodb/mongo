//
// Tests initialization of an empty cluster with multiple mongoses.
// Starts a bunch of mongoses in parallel, and ensures that there's only a single config 
// version initialization.
//

jsTest.log("Start config servers...")

var configSvrA = MongoRunner.runMongod({ configsvr: "", verbose : 2 });
var configSvrB = MongoRunner.runMongod({ configsvr: "", verbose : 2 });
var configSvrC = MongoRunner.runMongod({ configsvr: "", verbose : 2 });

var configConnStr = [configSvrA.host, configSvrB.host, configSvrC.host].join(",");

var configConn = configSvrA;

// Start profiling the config db
configConn.getDB("config").setProfilingLevel(2);

//
// Start a bunch of mongoses which will probably interfere
//

jsTest.log("Starting first set of mongoses in parallel...");

var mongoses = [];
for (var i = 0; i < 3; i++) {
    var mongos = MongoRunner.runMongos({ binVersion : "latest", 
                                         configdb : configConnStr,
                                         waitForConnect : false });
    
    mongoses.push(mongos);
}

// Eventually connect to a mongo host, to be sure that the config upgrade happened
// (This can take longer on extremely slow bbots or VMs)
var mongosConn = null;
assert.soon(function() {
    try {
        mongosConn = new Mongo(mongoses[0].host);
        return true;
    }
    catch (e) {
        print("Waiting for connect...");
        printjson(e);
        return false;
    }
}, "Mongos " + mongoses[0].host + " did not start.", 5 * 60 * 1000 );

var version = mongosConn.getCollection("config.version").findOne();

//
// Start a second set of mongoses which should respect the initialized version
//

jsTest.log("Starting second set of mongoses...");

for (var i = 0; i < 3; i++) {
    var mongos = MongoRunner.runMongos({ binVersion : "latest", 
                                         configdb : configConnStr,
                                         waitForConnect : false });
    
    mongoses.push(mongos);
}

// Eventually connect to a host
assert.soon(function() {
    try {
        mongosConn = new Mongo(mongoses[mongoses.length - 1].host);
        return true;
    }
    catch (e) {
        print("Waiting for connect...");
        printjson(e);
        return false;
    }
}, "Later mongos " + mongoses[ mongoses.length - 1 ].host + " did not start.", 5 * 60 * 1000 );

// Shut down our mongoses now that we've tested them
for (var i = 0; i < mongoses.length; i++) {
    MongoRunner.stopMongos(mongoses[i]);
}

jsTest.log("Mongoses stopped...");

//
// Check version and that the version was only updated once
//

printjson(version);

assert.eq(version.minCompatibleVersion, 5);
assert.eq(version.currentVersion, 6);
assert(version.clusterId);
assert.eq(version.excluding, undefined);

jsTest.log("Ensuring config.version collection only written once...");

var updates = configConn.getDB("config").system.profile.find({ op : "update", 
                                                               ns : "config.version" }).toArray();
printjson(updates);
assert.eq(updates.length, 1);

MongoRunner.stopMongod(configSvrA);
MongoRunner.stopMongod(configSvrB);
MongoRunner.stopMongod(configSvrC);

jsTest.log("DONE!");


