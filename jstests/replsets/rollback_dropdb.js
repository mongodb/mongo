// test that a rollback of dropdatabase will cause the node to go into a FATAL state
//
// If all data-bearing nodes in a replica set are using an ephemeral storage engine, the set will
// not be able to survive a scenario where all data-bearing nodes are down simultaneously. In such a
// scenario, none of the members will have any data, and upon restart will each look for a member to
// inital sync from, so no primary will be elected. This test induces such a scenario, so cannot be
// run on ephemeral storage engines.
// @tags: [requires_persistence]

// set up a set and grab things for later
var name = "rollback_dropdb";
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

// drop database which should cause FATAL when rolled back
master = replTest.getPrimary();
assert(b_conn.host === master.host, "b_conn assumed to be master");
b_conn.getDB(name).dropDatabase();
assert.eq(0, b_conn.getDB(name).foo.find().itcount(), "dropping database failed");

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

// restart B, which should attempt rollback but then fassert
clearRawMongoProgramOutput();
replTest.restart(BID);
assert.soon(function() {
    return rawMongoProgramOutput().match(
        "rollback : can't rollback drop database full resync will be required");
}, "B failed to fassert");

replTest.stopSet(undefined, undefined, {allowedExitCodes: [MongoRunner.EXIT_ABRUPT]});
