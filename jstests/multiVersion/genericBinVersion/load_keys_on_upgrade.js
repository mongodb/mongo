import "jstests/multiVersion/libs/multi_rs.js";

import {ReplSetTest} from "jstests/libs/replsettest.js";

let oldVersion = "last-lts";

let nodes = {
    n1: {binVersion: oldVersion},
    n2: {binVersion: oldVersion},
    n3: {binVersion: oldVersion},
};

let keyFile = "jstests/libs/key1";
let rst = new ReplSetTest({nodes: nodes, keyFile: keyFile});

rst.startSet();

rst.initiate(Object.extend(rst.getReplSetConfig(), {writeConcernMajorityJournalDefault: true}), null, {
    initiateWithDefaultElectionTimeout: true,
});

// Wait for a primary node...
let primary = rst.getPrimary();

primary.getDB("admin").createUser({user: "root", pwd: "root", roles: ["root"]}, {w: 3});

let rsConn = new Mongo(rst.getURL());
assert.eq(1, rsConn.getDB("admin").auth("root", "root"));
assert.commandWorked(rsConn.adminCommand({hello: 1}));
print("clusterTime: " + tojson(rsConn.getDB("admin").getSession().getClusterTime()));

jsTest.log("Upgrading replica set...");

TestData.auth = true;
TestData.keyFile = keyFile;
TestData.authUser = "__system";
TestData.keyFileData = "foopdedoop";
TestData.authenticationDatabase = "local";
rst.upgradeSet({keyFile: keyFile, binVersion: "latest"});

jsTest.log("Replica set upgraded.");

TestData.keyFile = undefined;

try {
    rsConn.adminCommand({hello: 1});
} catch (e) {}

assert.eq(1, rsConn.getDB("admin").auth("root", "root"));
assert.commandWorked(rsConn.adminCommand({hello: 1}));
print("clusterTime2: " + tojson(rsConn.getDB("admin").getSession().getClusterTime()));

rst.stopSet();
