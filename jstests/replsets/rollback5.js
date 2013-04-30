// test that a rollback directory is created during a replica set rollback
// this also tests that updates are recorded in the rollback file
//  (this test does no delete rollbacks)

var replTest = new ReplSetTest({ name: 'rollback5', nodes: 3 });
var nodes = replTest.nodeList();

var conns = replTest.startSet();
var r = replTest.initiate({ "_id": "rollback5",
                            "members": [
                                { "_id": 0, "host": nodes[0] },
                                { "_id": 1, "host": nodes[1] },
                                { "_id": 2, "host": nodes[2], arbiterOnly: true}]
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
var Apath = "/data/db/rollback5-0/";
var Bpath = "/data/db/rollback5-1/";
assert(master == conns[0], "conns[0] assumed to be master");
assert(a_conn.host == master.host);

// Make sure we have an arbiter
assert.soon(function () {
    res = conns[2].getDB("admin").runCommand({ replSetGetStatus: 1 });
    return res.myState == 7;
}, "Arbiter failed to initialize.");

A.foo.update({key:'value1'}, {$set: {req: 'req'}}, true);
A.foo.runCommand({getLastError : 1, w : 2, wtimeout : 60000});
replTest.stop(AID);

master = replTest.getMaster();
assert(b_conn.host == master.host);
B.foo.update({key:'value1'}, {$set: {res: 'res'}}, true);
B.foo.runCommand({getLastError : 1, w : 1, wtimeout : 60000});
replTest.stop(BID);
replTest.restart(AID);
master = replTest.getMaster();
assert(a_conn.host == master.host);
A.foo.update({key:'value2'}, {$set: {req: 'req'}}, true);
A.foo.runCommand({getLastError : 1, w : 1, wtimeout : 60000});
replTest.restart(BID); // should rollback
reconnect(B);

print("BEFORE------------------");
printjson(A.foo.find().toArray());

replTest.awaitReplication();
replTest.awaitSecondaryNodes();

print("AFTER------------------");
printjson(A.foo.find().toArray());

assert.eq(2, A.foo.count());
assert.eq('req', A.foo.findOne({key:'value1'}).req);
assert.eq(null, A.foo.findOne({key:'value1'}).res);
assert.eq(2, B.foo.count());
assert.eq('req', B.foo.findOne({key:'value1'}).req);
assert.eq(null, B.foo.findOne({key:'value1'}).res);

// check here for rollback files
var rollbackDir = Bpath + "rollback/";
assert(pathExists(rollbackDir), "rollback directory was not created!");

print("rollback5.js SUCCESS");
replTest.stopSet(15);


function wait(f) {
    var n = 0;
    while (!f()) {
        if (n % 4 == 0)
            print("rollback5.js waiting");
        if (++n == 4) {
            print("" + f);
        }
        assert(n < 200, 'tried 200 times, giving up');
        sleep(1000);
    }
}

function reconnect(a) {
  wait(function() {
      try {
        a.bar.stats();
        return true;
      } catch(e) {
        print(e);
        return false;
      }
    });
};

