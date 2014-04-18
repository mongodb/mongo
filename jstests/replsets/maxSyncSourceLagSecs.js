// Test that setting maxSyncSourceLagSecs causes the set to change sync target
var replTest = new ReplSetTest({ nodes: 3, oplogSize: 5,
                                 nodeOptions: {setParameter: "maxSyncSourceLagSecs=3"}});
replTest.startSet();
replTest.initiate();

var master = replTest.getMaster();
master.getDB("foo").bar.save({a: 1});
replTest.awaitReplication();
var slaves = replTest.liveNodes.slaves;

// need to put at least maxSyncSourceLagSecs b/w first op and subsequent ops
// so that the shouldChangeSyncSource logic goes into effect
sleep(3000);

jsTestLog("Setting sync target of slave 2 to slave 1");
assert.commandWorked(slaves[1].getDB("admin").runCommand({replSetSyncFrom: slaves[0].name}));
assert.soon(function() {
        return (replTest.status().members[2].syncingTo == slaves[0].name);
    }, "sync target not changed to other slave");
printjson(replTest.status());

jsTestLog("Lock slave 1 and add some docs.  Force sync target for slave 2 to change to primary");
assert.commandWorked(slaves[0].getDB("admin").runCommand({fsync:1, lock: 1}));
master.getDB("foo").bar.save({a: 2});

assert.soon(function() {
        return (replTest.status().members[2].syncingTo == master.name);
    }, "sync target not changed back to primary");
printjson(replTest.status());

assert.soon(function() {
        return (slaves[1].getDB("foo").bar.count() == 2);
    }, "slave should have caught up after syncing to primary.");

assert.commandWorked(slaves[0].getDB("admin").$cmd.sys.unlock.findOne());
replTest.stopSet();
