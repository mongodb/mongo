// test that a rollback directory is created during a replica set rollback
// this also tests that updates are recorded in the rollback file
//  (this test does no delete rollbacks)
//
// If all data-bearing nodes in a replica set are using an ephemeral storage engine, the set will
// not be able to survive a scenario where all data-bearing nodes are down simultaneously. In such a
// scenario, none of the members will have any data, and upon restart will each look for a member to
// inital sync from, so no primary will be elected. This test induces such a scenario, so cannot be
// run on ephemeral storage engines.
// @tags: [requires_persistence]

var replTest = new ReplSetTest({name: 'rollback5', nodes: 3});
var nodes = replTest.nodeList();

var conns = replTest.startSet();
var r = replTest.initiate({
    "_id": "rollback5",
    "members": [
        {"_id": 0, "host": nodes[0], priority: 3},
        {"_id": 1, "host": nodes[1]},
        {"_id": 2, "host": nodes[2], arbiterOnly: true}
    ]
});

// Make sure we have a master
replTest.waitForState(replTest.nodes[0], ReplSetTest.State.PRIMARY);
var master = replTest.getPrimary();
var a_conn = conns[0];
var b_conn = conns[1];
a_conn.setSlaveOk();
b_conn.setSlaveOk();
var A = a_conn.getDB("test");
var B = b_conn.getDB("test");
var AID = replTest.getNodeId(a_conn);
var BID = replTest.getNodeId(b_conn);
var Apath = MongoRunner.dataDir + "/rollback5-0/";
var Bpath = MongoRunner.dataDir + "/rollback5-1/";
assert(master == conns[0], "conns[0] assumed to be master");
assert(a_conn.host == master.host);

// Make sure we have an arbiter
assert.soon(function() {
    res = conns[2].getDB("admin").runCommand({replSetGetStatus: 1});
    return res.myState == 7;
}, "Arbiter failed to initialize.");

var options = {writeConcern: {w: 2, wtimeout: 60000}, upsert: true};
assert.writeOK(A.foo.update({key: 'value1'}, {$set: {req: 'req'}}, options));
replTest.stop(AID);

master = replTest.getPrimary();
assert(b_conn.host == master.host);
options = {
    writeConcern: {w: 1, wtimeout: 60000},
    upsert: true
};
assert.writeOK(B.foo.update({key: 'value1'}, {$set: {res: 'res'}}, options));
replTest.stop(BID);
replTest.restart(AID);
master = replTest.getPrimary();
assert(a_conn.host == master.host);
options = {
    writeConcern: {w: 1, wtimeout: 60000},
    upsert: true
};
assert.writeOK(A.foo.update({key: 'value2'}, {$set: {req: 'req'}}, options));
replTest.restart(BID);  // should rollback
reconnect(B);

print("BEFORE------------------");
printjson(A.foo.find().toArray());

replTest.awaitReplication();
replTest.awaitSecondaryNodes();

print("AFTER------------------");
printjson(A.foo.find().toArray());

assert.eq(2, A.foo.find().itcount());
assert.eq('req', A.foo.findOne({key: 'value1'}).req);
assert.eq(null, A.foo.findOne({key: 'value1'}).res);
reconnect(B);
assert.eq(2, B.foo.find().itcount());
assert.eq('req', B.foo.findOne({key: 'value1'}).req);
assert.eq(null, B.foo.findOne({key: 'value1'}).res);

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
        } catch (e) {
            print(e);
            return false;
        }
    });
}
