// This tests that:
// * stale members get into state 3 (recovering)
// * they stay in state 3 after restarting
// * they can recover and go into state 2 if someone less up-to-date becomes primary
//
// This test requires persistence in order for a restarted node with a stale oplog to stay in the
// RECOVERING state. A restarted node with an ephemeral storage engine will not have an oplog upon
// restart, so will immediately resync.
// @tags: [requires_persistence]

/**
 * 1: initial insert
 * 2: initial sync
 * 3: blind s2
 * 4: overflow oplog
 * 5: unblind s2
 * 6: check s2.state == 3
 * 7: restart s2
 * 8: check s2.state == 3
 */

var w = 0;
var wait = function(f) {
    w++;
    var n = 0;
    while (!f()) {
        if (n % 4 == 0)
            print("toostale.js waiting " + w);
        if (++n == 4) {
            print("" + f);
        }
        assert(n < 200, 'tried 200 times, giving up');
        sleep(1000);
    }
};

var reconnect = function(a) {
    wait(function() {
        try {
            a.bar.stats();
            return true;
        } catch (e) {
            print(e);
            return false;
        }
    });
};

var name = "toostale";
var replTest = new ReplSetTest({name: name, nodes: 3, oplogSize: 5});
var host = getHostName();

var nodes = replTest.startSet();
replTest.initiate({
    _id: name,
    members: [
        {_id: 0, host: host + ":" + replTest.ports[0], priority: 2},
        {_id: 1, host: host + ":" + replTest.ports[1], arbiterOnly: true},
        {_id: 2, host: host + ":" + replTest.ports[2], priority: 0}
    ]
});
var master = replTest.getPrimary();
var mdb = master.getDB("foo");

print("1: initial insert");
mdb.foo.save({a: 1000});

print("2: initial sync");
replTest.awaitReplication();

print("3: stop s2");
replTest.stop(2);
print("waiting until the master knows the slave is blind");
assert.soon(function() {
    return master.getDB("admin").runCommand({replSetGetStatus: 1}).members[2].health == 0;
});
print("okay");

print("4: overflow oplog");
reconnect(master.getDB("local"));
var count = master.getDB("local").oplog.rs.find().itcount();
var prevCount = -1;
while (count > prevCount) {
    print("inserting 1000");
    var bulk = mdb.bar.initializeUnorderedBulkOp();
    for (var i = 0; i < 1000; i++) {
        bulk.insert({x: i, date: new Date(), str: "safkaldmfaksndfkjansfdjanfjkafa"});
    }
    assert.writeOK(bulk.execute());

    prevCount = count;
    replTest.awaitReplication();
    count = master.getDB("local").oplog.rs.find().itcount();
    print("count: " + count + " prev: " + prevCount);
}

print("5: restart s2");
replTest.restart(2);
print("waiting until the master knows the slave is not blind");
assert.soon(function() {
    return master.getDB("admin").runCommand({replSetGetStatus: 1}).members[2].health != 0;
});
print("okay");

print("6: check s2.state == 3");
var goStale = function() {
    wait(function() {
        var status = master.getDB("admin").runCommand({replSetGetStatus: 1});
        printjson(status);
        return status.members[2].state == 3;
    });
};
goStale();

print("7: restart s2");
replTest.stop(2);
replTest.restart(2);

print("8: check s2.state == 3");
assert.soon(function() {
    var status = master.getDB("admin").runCommand({replSetGetStatus: 1});
    printjson(status);
    return status.members && status.members[2].state == 3;
});

replTest.stop(0);
