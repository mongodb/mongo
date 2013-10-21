// test that the resync command works with replica sets and that one does not need to manually
// force a replica set resync by deleting all datafiles

var replTest = new ReplSetTest({name: 'resync', nodes: 3, oplogSize: 1});
var nodes = replTest.nodeList();

var conns = replTest.startSet();
var r = replTest.initiate({ "_id": "resync",
                            "members": [
                                {"_id": 0, "host": nodes[0]},
                                {"_id": 1, "host": nodes[1]},
                                {"_id": 2, "host": nodes[2]}]
                          });

// Make sure we have a master
var master = replTest.getMaster();
var a_conn = conns[0];
var b_conn = conns[1];
a_conn.setSlaveOk();
b_conn.setSlaveOk();
var A = a_conn.getDB("test");
var B = b_conn.getDB("test");
var AID = replTest.getNodeId(a_conn);
var BID = replTest.getNodeId(b_conn);
assert(master == conns[0], "conns[0] assumed to be master");
assert(a_conn.host == master.host);

// create an oplog entry with an insert
A.foo.insert({x:1});
A.foo.runCommand({getLastError : 1, w : 3, wtimeout : 60000});
replTest.stop(BID);

// insert enough to cycle oplog
for (i=2; i < 10000; i++) {
    A.foo.insert({x:i});
}
// wait for secondary to also have its oplog cycle
A.foo.runCommand({getLastError : 1, w : 2, wtimeout : 60000});

// bring node B and it will enter recovery mode because its newest oplog entry is too old
replTest.restart(BID);
// check that it is in recovery mode
assert.soon(function() {
    try {
        var result = b_conn.getDB("admin").runCommand({isMaster: 1});
        printjson(result);
        return !result.ismaster && !result.secondary;
    }
    catch ( e ) {
        print( e );
    }
});

// run resync and wait for it to happen
b_conn.getDB("admin").runCommand({resync:1});
replTest.awaitReplication();
replTest.awaitSecondaryNodes();

replTest.stopSet(15);
