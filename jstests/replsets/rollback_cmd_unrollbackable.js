/**
 * Test that a rollback of a non-rollbackable command causes a message to be logged
 *
 * If all data-bearing nodes in a replica set are using an ephemeral storage engine, the set will
 * not be able to survive a scenario where all data-bearing nodes are down simultaneously. In such a
 * scenario, none of the members will have any data, and upon restart will each look for a member to
 * initial sync from, so no primary will be elected. This test induces such a scenario, so cannot be
 * run on ephemeral storage engines.
 * @tags: [requires_persistence]
*/

// Sets up a replica set and grabs things for later.
var name = "rollback_cmd_unrollbackable";
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

// Gets master and do an initial write.
replTest.waitForState(replTest.nodes[0], ReplSetTest.State.PRIMARY);
var master = replTest.getPrimary();
assert(master === conns[0], "conns[0] assumed to be master");
assert(a_conn.host === master.host, "a_conn assumed to be master");
var options = {writeConcern: {w: 2, wtimeout: ReplSetTest.kDefaultTimeoutMS}, upsert: true};
assert.writeOK(a_conn.getDB(name).foo.insert({x: 1}, options));

// Shuts down the master.
replTest.stop(AID);

// Inserts a fake oplog entry with a non-rollbackworthy command.
master = replTest.getPrimary();
assert(b_conn.host === master.host, "b_conn assumed to be master");
options = {
    writeConcern: {w: 1, wtimeout: ReplSetTest.kDefaultTimeoutMS},
    upsert: true
};
// Inserts another oplog entry to set minValid ahead.
assert.writeOK(b_conn.getDB(name).foo.insert({x: 123}));
var oplog_entry = b_conn.getDB("local").oplog.rs.find().sort({$natural: -1})[0];
oplog_entry["ts"] = Timestamp(oplog_entry["ts"].t, oplog_entry["ts"].i + 1);
oplog_entry["op"] = "c";
oplog_entry["o"] = {
    "emptycapped": 1
};
assert.writeOK(b_conn.getDB("local").oplog.rs.insert(oplog_entry));

// Shuts down B and brings back the original master.
replTest.stop(BID);
replTest.restart(AID);
master = replTest.getPrimary();
assert(a_conn.host === master.host, "a_conn assumed to be master");

// Does a write so that B will have to roll back.
options = {
    writeConcern: {w: 1, wtimeout: ReplSetTest.kDefaultTimeoutMS},
    upsert: true
};
assert.writeOK(a_conn.getDB(name).foo.insert({x: 2}, options));

// Restarts B, which should attempt to rollback but then fassert.
clearRawMongoProgramOutput();
replTest.restart(BID);
var msg = RegExp("Can't roll back this command yet: ");
assert.soon(function() {
    return rawMongoProgramOutput().match(msg);
}, "Did not see a log entry about skipping the nonrollbackable command during rollback");

replTest.stop(BID, undefined, {allowedExitCode: MongoRunner.EXIT_ABRUPT});
replTest.stopSet();
