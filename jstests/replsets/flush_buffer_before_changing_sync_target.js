// Test for SERVER-8070: Flush buffer before changing sync targets to prevent unnecessary rollbacks
// This test writes 50 ops to one secondary's data (member2) and 25 ops to the other secondary's
// data (member3), then puts 50 more ops in member3's buffer and makes sure that member3 doesn't try
// to sync from member2.

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";
import {syncFrom} from "jstests/replsets/rslib.js";

// helper to ensure two nodes are at the same place in the oplog
let waitForSameOplogPosition = function (db1, db2, errmsg) {
    assert.soon(function () {
        let last1 = db1.getSiblingDB("local").oplog.rs.find().sort({$natural: -1}).limit(1).next();
        let last2 = db2.getSiblingDB("local").oplog.rs.find().sort({$natural: -1}).limit(1).next();
        jsTest.log("primary: " + tojson(last1) + " secondary: " + tojson(last2));

        return last1.ts.t === last2.ts.t && last1.ts.i === last2.ts.i;
    }, errmsg);
};

// start set
let replSet = new ReplSetTest({name: "testSet", nodes: 3});
replSet.startSet();
replSet.initiate({
    _id: "testSet",
    members: [
        {_id: 0, host: getHostName() + ":" + replSet.ports[0]},
        {_id: 1, host: getHostName() + ":" + replSet.ports[1], priority: 0},
        {_id: 2, host: getHostName() + ":" + replSet.ports[2], priority: 0},
    ],
    settings: {chainingAllowed: false},
});

// set up common points of access
let primary = replSet.getPrimary();
let primaryDB = primary.getDB("foo");
replSet.nodes[1].setSecondaryOk();
replSet.nodes[2].setSecondaryOk();
let member2 = replSet.nodes[1].getDB("admin");
let member3 = replSet.nodes[2].getDB("admin");

// The default WC is majority and stopServerReplication will prevent satisfying any majority writes.
assert.commandWorked(
    primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);

// Do an initial write
primary.getDB("foo").bar.insert({x: 1});
replSet.awaitReplication();

jsTest.log("Make sure 2 & 3 are syncing from the primary");
assert.eq(primary, replSet.nodes[0]);
syncFrom(replSet.nodes[1], primary, replSet);
syncFrom(replSet.nodes[2], primary, replSet);

jsTest.log("Stop 2's replication");
member2.runCommand({configureFailPoint: "rsSyncApplyStop", mode: "alwaysOn"});
checkLog.contains(member2, "rsSyncApplyStop fail point enabled. Blocking until fail point is disabled");

jsTest.log("Do a few writes");
for (var i = 0; i < 25; i++) {
    primaryDB.bar.insert({x: i});
}

jsTest.log("Make sure 3 is at write #25");
waitForSameOplogPosition(primaryDB, member3, "node 3 failed to catch up to the primary");
// This means 3's buffer is empty

jsTest.log("Stop 3's replication");
member3.runCommand({configureFailPoint: "rsSyncApplyStop", mode: "alwaysOn"});
checkLog.contains(member3, "rsSyncApplyStop fail point enabled. Blocking until fail point is disabled");
// logLevel 3 will allow us to see each op the secondary pulls from the primary so that we can
// determine whether or not all ops are actually being pulled
member3.runCommand({setParameter: 1, logLevel: 3});

jsTest.log("Start 2's replication");
member2.runCommand({configureFailPoint: "rsSyncApplyStop", mode: "off"});

jsTest.log("Do some writes");
for (var i = 25; i < 50; i++) {
    primaryDB.bar.insert({x: i});
}

jsTest.log("Make sure 2 is at write #50");
waitForSameOplogPosition(primaryDB, member2, "node 2 failed to catch up to the primary");
// This means 2's buffer is empty

jsTest.log("Stop 2's replication");
member2.runCommand({configureFailPoint: "rsSyncApplyStop", mode: "alwaysOn"});
checkLog.contains(member2, "rsSyncApplyStop fail point enabled. Blocking until fail point is disabled");

jsTest.log("Do some writes - 2 & 3 should have up to write #75 in their buffers, but unapplied");
for (var i = 50; i < 75; i++) {
    primaryDB.bar.insert({x: i});
}
let primaryCollectionSize = primaryDB.bar.find().itcount();
jsTest.log("primary collection size: " + primaryCollectionSize);
let last = primaryDB.getSiblingDB("local").oplog.rs.find().sort({$natural: -1}).limit(1).next();

jsTest.log("waiting a bit for the secondaries to get the write");
sleep(10000);

jsTest.log("Shut down the primary");
replSet.stop(0);

// make sure 3 doesn't try to sync from 2
// the sleep 30sec is a hold over from the unsafe assert.throws(assert.soon())
// which would check for 30 seconds that node 3 didn't try to sync from 2
sleep(30 * 1000);
jsTest.log("3 should not attempt to sync from 2, as it cannot clear its buffer");
let syncSourceHost = member3.adminCommand({replSetGetStatus: 1}).syncSourceHost;
assert(syncSourceHost !== getHostName() + ":" + replSet.ports[1], "node 3 is syncing from node 2 :(");

jsTest.log("Pause 3's bgsync thread");
stopServerReplication(member3.getMongo());

// count documents in member 3
assert.eq(
    26,
    member3.getSiblingDB("foo").bar.find().itcount(),
    "collection size incorrect on node 3 before applying ops 25-75",
);

jsTest.log("Allow 3 to apply ops 25-75");
assert.commandWorked(
    member3.runCommand({configureFailPoint: "rsSyncApplyStop", mode: "off"}),
    "member 3 rsSyncApplyStop admin command failed",
);

assert.soon(function () {
    let last3 = member3.getSiblingDB("local").oplog.rs.find().sort({$natural: -1}).limit(1).next();
    jsTest.log("primary: " + tojson(last, "", true) + " secondary: " + tojson(last3, "", true));
    jsTest.log("member 3 collection size: " + member3.getSiblingDB("foo").bar.find().itcount());
    jsTest.log("curop: ");
    printjson(member3.getSiblingDB("foo").currentOp(true));
    return last.ts.t === last3.ts.t && last.ts.i === last3.ts.i;
}, "Replication member 3 did not apply ops 25-75");

jsTest.log("Start 3's bgsync thread");
restartServerReplication(member3.getMongo());

jsTest.log("Node 3 shouldn't hit rollback");
let end = new Date().getTime() + 10000;
while (new Date().getTime() < end) {
    assert("ROLLBACK" !== member3.runCommand({replSetGetStatus: 1}).members[2].stateStr);
    sleep(30);
}

// Need to re-enable writes before clean shutdown.
assert.commandWorked(member2.runCommand({configureFailPoint: "rsSyncApplyStop", mode: "off"}));

replSet.stopSet();
