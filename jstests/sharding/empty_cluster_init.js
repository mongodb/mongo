//
// Tests initialization of an empty cluster with multiple mongoses.
// Starts a bunch of mongoses in parallel, and ensures that there's only a single config
// version initialization.
//
// This test manually starts routers with the latest binary which can't connect to config server
// nodes using the last-lts binary because their wire versions are incompatible.
// @tags: [multiversion_incompatible]
//

import {ReplSetTest} from "jstests/libs/replsettest.js";

let configRS = new ReplSetTest({name: "configRS", nodes: 3, useHostName: true});
configRS.startSet({configsvr: "", storageEngine: "wiredTiger"});
let replConfig = configRS.getReplSetConfig();
replConfig.configsvr = true;
configRS.initiate(replConfig);

//
// Start a bunch of mongoses which will probably interfere
//

jsTest.log("Starting first set of mongoses in parallel...");

let mongoses = [];
for (var i = 0; i < 3; i++) {
    var mongos = MongoRunner.runMongos({binVersion: "latest", configdb: configRS.getURL(), waitForConnect: false});
    mongoses.push(mongos);
}

// Eventually connect to a mongo host, to be sure that the config upgrade happened
// (This can take longer on extremely slow bbots or VMs)
let mongosConn = null;
assert.soon(
    function () {
        try {
            mongosConn = new Mongo(mongoses[0].host);
            return true;
        } catch (e) {
            print("Waiting for connect...");
            printjson(e);
            return false;
        }
    },
    "Mongos " + mongoses[0].host + " did not start.",
    5 * 60 * 1000,
);

let version = mongosConn.getCollection("config.version").findOne();

//
// Start a second set of mongoses which should respect the initialized version
//

jsTest.log("Starting second set of mongoses...");

for (var i = 0; i < 3; i++) {
    var mongos = MongoRunner.runMongos({binVersion: "latest", configdb: configRS.getURL(), waitForConnect: false});
    mongoses.push(mongos);
}

let connectToMongos = function (host) {
    // Eventually connect to a host
    assert.soon(
        function () {
            try {
                mongosConn = new Mongo(host);
                return true;
            } catch (e) {
                print("Waiting for connect to " + host);
                printjson(e);
                return false;
            }
        },
        "mongos " + host + " did not start.",
        5 * 60 * 1000,
    );
};

for (var i = 0; i < mongoses.length; i++) {
    connectToMongos(mongoses[i].host);
}

// Shut down our mongoses now that we've tested them
for (var i = 0; i < mongoses.length; i++) {
    MongoRunner.stopMongos(mongoses[i]);
}

//
// Check version and that the version was only updated once
//

assert.hasFields(version, ["clusterId"], "Version document does not contain cluster ID");

let oplog = configRS.getPrimary().getDB("local").oplog.rs;
let updates = oplog.find({ns: "config.version"}).toArray();
assert.eq(1, updates.length, "ops to config.version: " + tojson(updates));

configRS.stopSet(15);
