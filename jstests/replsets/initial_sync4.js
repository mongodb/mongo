// Test update modifier uassert during initial sync. SERVER-4781

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {reconnect, wait} from "jstests/replsets/rslib.js";

let basename = "jstests_initsync4";

jsTestLog("1. Bring up set");
let replTest = new ReplSetTest({name: basename, nodes: 1});
replTest.startSet();
replTest.initiate();

let m = replTest.getPrimary();
let md = m.getDB("d");
let mc = m.getDB("d")["c"];

jsTestLog("2. Insert some data");
let N = 5000;
mc.createIndex({x: 1});
let bulk = mc.initializeUnorderedBulkOp();
for (var i = 0; i < N; ++i) {
    bulk.insert({_id: i, x: i, a: {}});
}
assert.commandWorked(bulk.execute());

jsTestLog("3. Make sure synced");
replTest.awaitReplication();

jsTestLog("4. Bring up a new node");
let hostname = getHostName();

let s = MongoRunner.runMongod({replSet: basename, oplogSize: 2});

let config = replTest.getReplSetConfig();
config.version = replTest.getReplSetConfigFromNode().version + 1;
config.members.push({_id: 2, host: hostname + ":" + s.port, priority: 0, votes: 0});
try {
    m.getDB("admin").runCommand({replSetReconfig: config});
} catch (e) {
    print(e);
}
reconnect(s);
assert.eq(m, replTest.getPrimary(), "Primary changed after reconfig");

jsTestLog("5. Wait for new node to start cloning");

s.setSecondaryOk();
let sc = s.getDB("d")["c"];

wait(function () {
    printjson(sc.stats());
    return sc.stats().count > 0;
});

jsTestLog("6. Start updating documents on primary");
for (i = N - 1; i >= N - 10000; --i) {
    // If the document is cloned as {a:1}, the {$set:{'a.b':1}} modifier will uassert.
    mc.update({_id: i}, {$set: {"a.b": 1}}, {writeConcern: {w: 1}});
    mc.update({_id: i}, {$set: {a: 1}}, {writeConcern: {w: 1}});
}

for (i = N; i < N * 2; i++) {
    mc.insert({_id: i, x: i}, {writeConcern: {w: 1}});
}

assert.eq(N * 2, mc.find().itcount());

jsTestLog("7. Wait for new node to become SECONDARY");
wait(function () {
    let status = s.getDB("admin").runCommand({replSetGetStatus: 1});
    printjson(status);
    return status.members && status.members[1].state == 2;
});

jsTestLog("8. Wait for new node to have all the data");
wait(function () {
    return sc.find().itcount() == mc.find().itcount();
});

assert.eq(mc.getIndexKeys().length, sc.getIndexKeys().length);

assert.eq(mc.find().sort({x: 1}).itcount(), sc.find().sort({x: 1}).itcount());
MongoRunner.stopMongod(s);
replTest.stopSet(15);
