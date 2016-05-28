// test that a rollback of an entry that matches the ts but not h causes no problems
//
// If all data-bearing nodes in a replica set are using an ephemeral storage engine, the set will
// not be able to survive a scenario where all data-bearing nodes are down simultaneously. In such a
// scenario, none of the members will have any data, and upon restart will each look for a member to
// inital sync from, so no primary will be elected. This test induces such a scenario, so cannot be
// run on ephemeral storage engines.
// @tags: [requires_persistence]

// function to check the logs for an entry
doesEntryMatch = function(array, regex) {
    var found = false;
    for (i = 0; i < array.length; i++) {
        if (regex.test(array[i])) {
            found = true;
        }
    }
    return found;
};

// set up a set and grab things for later
var name = "rollback_different_h";
var replTest = new ReplSetTest({name: name, nodes: 3});
var nodes = replTest.nodeList();
var conns = replTest.startSet();
replTest.initiate({
    "_id": name,
    "members": [
        {"_id": 0, "host": nodes[0], priority: 3},
        {"_id": 1, "host": nodes[1]},
        {"_id": 2, "host": nodes[2], arbiterOnly: true}
    ]
});
var a_conn = conns[0];
var b_conn = conns[1];
var AID = replTest.getNodeId(a_conn);
var BID = replTest.getNodeId(b_conn);

replTest.waitForState(replTest.nodes[0], ReplSetTest.State.PRIMARY, 60 * 1000);

// get master and do an initial write
var master = replTest.getPrimary();
assert(master === conns[0], "conns[0] assumed to be master");
assert(a_conn.host === master.host, "a_conn assumed to be master");
var options = {writeConcern: {w: 2, wtimeout: 60000}, upsert: true};
assert.writeOK(a_conn.getDB(name).foo.insert({x: 1}, options));

// shut down master
replTest.stop(AID);

// change the h value of the most recent entry on B
master = replTest.getPrimary();
assert(b_conn.host === master.host, "b_conn assumed to be master");
options = {
    writeConcern: {w: 1, wtimeout: 60000},
    upsert: true
};
var oplog_entry = b_conn.getDB("local").oplog.rs.find().sort({$natural: -1})[0];
oplog_entry["ts"].t++;
oplog_entry["h"] = NumberLong(1);
res = b_conn.getDB("local").oplog.rs.insert(oplog_entry);
assert(res.nInserted > 0, tojson(res));

// another insert to set minvalid ahead
assert.writeOK(b_conn.getDB(name).foo.insert({x: 123}));

// shut down B and bring back the original master
replTest.stop(BID);
replTest.restart(AID);
master = replTest.getPrimary();
assert(a_conn.host === master.host, "a_conn assumed to be master");

// do a write so that B will have to roll back
options = {
    writeConcern: {w: 1, wtimeout: 60000},
    upsert: true
};
assert.writeOK(a_conn.getDB(name).foo.insert({x: 2}, options));

// restart B, which should rollback and get to the same state as A
replTest.restart(BID);
assert.soon(function() {
    try {
        var collb = b_conn.getDB(name).foo.find().toArray();
        var colla = a_conn.getDB(name).foo.find().toArray();
        for (var i = 0; i < colla.length; i++) {
            if (tojson(colla[i]) !== tojson(collb[i])) {
                return false;
            }
        }
        return true;
    } catch (e) {
        return false;
    }
}, "collection on A and B did not match after rollback");

replTest.stopSet();
