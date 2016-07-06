// test that a rollback of an index creation op caused the index to be dropped.
//
// If all data-bearing nodes in a replica set are using an ephemeral storage engine, the set will
// not be able to survive a scenario where all data-bearing nodes are down simultaneously. In such a
// scenario, none of the members will have any data, and upon restart will each look for a member to
// inital sync from, so no primary will be elected. This test induces such a scenario, so cannot be
// run on ephemeral storage engines.
// @tags: [requires_persistence]

load("jstests/replsets/rslib.js");

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
var name = "rollback_index";
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

// Create a unique index that, if not dropped during rollback, would
// cause errors when applying operations from the primary.
master = replTest.getPrimary();
assert(b_conn.host === master.host, "b_conn assumed to be master");
options = {
    writeConcern: {w: 1, wtimeout: 60000},
    upsert: true
};
// another insert to set minvalid ahead
assert.writeOK(b_conn.getDB(name).foo.insert({x: 123}));
assert.commandWorked(b_conn.getDB(name).foo.ensureIndex({x: 1}, {unique: true}));
assert.writeError(b_conn.getDB(name).foo.insert({x: 123}));

// shut down B and bring back the original master
replTest.stop(BID);
replTest.restart(AID);
master = replTest.getPrimary();
assert(a_conn.host === master.host, "a_conn assumed to be master");

// Insert a document with the same value for 'x' that should be
// propagated successfully to B if the unique index was dropped successfully.
options = {
    writeConcern: {w: 1, wtimeout: 60000}
};
assert.writeOK(a_conn.getDB(name).foo.insert({x: 1}, options));
assert.eq(2, a_conn.getDB(name).foo.find().itcount(), 'invalid number of documents on A');

// restart B, which should rollback.
replTest.restart(BID);

awaitOpTime(b_conn, getLatestOp(a_conn).ts);
replTest.awaitReplication();
replTest.awaitSecondaryNodes();

// Perform a write that should succeed if there's no unique index on B.
options = {
    writeConcern: {w: 'majority', wtimeout: 60000}
};
assert.writeOK(a_conn.getDB(name).foo.insert({x: 1}, options));

// Check collections and indexes.
assert.eq(3,
          b_conn.getDB(name).foo.find().itcount(),
          'Collection on B does not have the same number of documents as A');
assert.eq(
    a_conn.getDB(name).foo.getIndexes().length,
    b_conn.getDB(name).foo.getIndexes().length,
    'Unique index not dropped during rollback: ' + tojson(b_conn.getDB(name).foo.getIndexes()));

replTest.stopSet();
