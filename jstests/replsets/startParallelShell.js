// Test startParallelShell() in a replica set.

import {ReplSetTest} from "jstests/libs/replsettest.js";

const setName = 'rs0';
const replSet = new ReplSetTest({name: setName, nodes: 3});
const nodes = replSet.nodeList();
replSet.startSet();
replSet.initiate();

const url = replSet.getURL();
print("* Connecting to " + url);
const mongo = new Mongo(url);
globalThis.db = mongo.getDB('admin');  // `startParallelShell` has custom logic for `globalThis.db`
assert.eq(url, mongo.host, "replSet.getURL() should match active connection string");

print("* Starting parallel shell on --host " + db.getMongo().host);
var awaitShell = startParallelShell('db.coll0.insert({test: "connString only"});');
assert.soon(function() {
    return db.coll0.find({test: "connString only"}).count() === 1;
});
awaitShell();

const uri = new MongoURI(url);
const port0 = uri.servers[0].port;
print("* Starting parallel shell w/ --port " + port0);
awaitShell = startParallelShell('db.coll0.insert({test: "explicit port"});', port0);
assert.soon(function() {
    return db.coll0.find({test: "explicit port"}).count() === 1;
});
awaitShell();
replSet.stopSet();
