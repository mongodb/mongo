// test that nodes still respond to heartbeats during compact() SERVER-12264
var replTest = new ReplSetTest({ name: 'compact', nodes: 3 });

replTest.startSet();
replTest.initiate();

var master = replTest.getMaster();
var compactingSlave = replTest.liveNodes.slaves[0];

// populate data
for (i=0; i<1000; i++) {
    var bulkInsertArr = [];
    for (j=0; j<1000; j++) {
        bulkInsertArr.push({x:i, y:j});
    }
    master.getDB("compact").foo.insert(bulkInsertArr);
}

// takes a while to replicate all this data...
replTest.awaitReplication(1000*60*5);

// run compact in parallel with this rest of the script
var cmd = "tojson(db.getSiblingDB('compact').runCommand({'compact': 'foo', 'paddingFactor': 2}));";
var compactor = startParallelShell(cmd, compactingSlave.port);;

// wait for compact to show up in currentOp
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

// then check that it is still responding to heartbeats
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

replTest.stopSet();
