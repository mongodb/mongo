// test that nodes still respond to heartbeats (SERVER-12264) and auth requests (SERVER-12359)
// during the compact command

// this function will wait until compact has acquired the WriteLock and then return
awaitCompact = function() {
    print("waiting for compact to acquire lock");
    assert.soon(function() {
        var curop = compactingSlave.getDB('compact').currentOp();
        for (index in curop.inprog) {
            entry = curop.inprog[index];
            if (entry.query.hasOwnProperty("compact") && entry.query.compact === "foo"
                && entry.hasOwnProperty("locks") && entry.locks.hasOwnProperty("^compact")
                && entry.locks["^compact"] === "W") {
                return true;
            }
        }
        return false;
    }, "compact didn't start in 30 seconds", 30*1000, 100);
};

// set up replSet
var replTest = new ReplSetTest({name: 'compact', nodes: 3, keyFile: "jstests/libs/key1"});
replTest.startSet();
replTest.initiate();
var master = replTest.getMaster();

// populate data
for (i=0; i<1000; i++) {
    var bulkInsertArr = [];
    for (j=0; j<1000; j++) {
        bulkInsertArr.push({x:i, y:j});
    }
    master.getDB("compact").foo.insert(bulkInsertArr);
}

// takes a while to replicate all this data...
replTest.awaitReplication(1000*60*10);

// run compact in parallel
var compactingSlave = replTest.liveNodes.slaves[0];
print("starting first compact on port: " + compactingSlave);
var cmd = "printjson(db.getSiblingDB('compact').runCommand({'compact': 'foo', 'paddingFactor': 2}));";
var compactor = startParallelShell(cmd, compactingSlave.port);

// wait for compact to have lock and then see if heartbeats work
awaitCompact();
for (i=0; i<5; i++) {
    var start = new Date();
    var result = compactingSlave.getDB("admin").runCommand({"replSetHeartbeat": "compact",
                                                            "v": NumberInt(1),
                                                            "pv": NumberInt(1),
                                                            "checkEmpty": false,
                                                            "fromId": NumberInt(0)
                                                            });
    var end = new Date();
    assert.eq(result.ok, 1, "heartbeat didn't return properly");
    // wait for 10 seconds because that's how long it takes for a node to be considered down
    assert.lt(end - start, 10 * 1000, "heartbeat didn't return quickly enough");
}
print("heartbeat worked during compact");

// wait for the first compact to finish
compactor();

// setup auth and auth to all
master.getDB("admin").createUser({user:"admin", pwd:"hunter2", roles:["root"]}, {w:2});
// have to wait til auth info replicates, but can't awaitReplication() because some expect auth...
assert.soon(function() {
    return master.getDB("admin").auth({user:"admin", pwd:"hunter2"})
           && compactingSlave.getDB("admin").auth({user:"admin", pwd:"hunter2"})
           && replTest.liveNodes.slaves[1].getDB("admin").auth({user:"admin", pwd:"hunter2"})
});

// auth and run compact in parallel
print("starting second compact");
cmd = "printjson(db.getSiblingDB('admin').auth('admin','hunter2'));"
      + " printjson(db.getSiblingDB('compact').runCommand({'compact': 'foo', 'paddingFactor': 2}));";
var compactor2 = startParallelShell(cmd, compactingSlave.port);

// wait for compact to have lock and then check if auth works
awaitCompact();
for (i=0; i<5; i++) {
    var start = new Date();
    var result = compactingSlave.getDB("admin").auth({user:"admin", pwd:"hunter"+i});
    var end = new Date();
    assert.lt(end - start, 2 * 1000, "auth didn't return quickly enough");
}
print("auth worked during compact");

// wait for the second compact to finish
print("finished!");
replTest.stopSet();
