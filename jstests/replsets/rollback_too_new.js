// test that a rollback of an op more than 1800 secs newer than the new master causes fatal shutdown
//
// If all data-bearing nodes in a replica set are using an ephemeral storage engine, the set will
// not be able to survive a scenario where all data-bearing nodes are down simultaneously. In such a
// scenario, none of the members will have any data, and upon restart will each look for a member to
// inital sync from, so no primary will be elected. This test induces such a scenario, so cannot be
// run on ephemeral storage engines.
// @tags: [requires_persistence]

(function() {
    "use strict";
    load("jstests/replsets/rslib.js");  // For getLatestOp()

    // set up a set and grab things for later
    var name = "rollback_too_new";
    var replTest = new ReplSetTest({name: name, nodes: 3});
    var nodes = replTest.nodeList();
    var conns = replTest.startSet();
    replTest.initiate({
        "_id": name,
        "members": [
            {"_id": 0, "host": nodes[0]},
            {"_id": 1, "host": nodes[1], arbiterOnly: true},
            {"_id": 2, "host": nodes[2], priority: 0}
        ],
        "settings": {"chainingAllowed": false}
    });
    var c_conn = conns[2];
    var CID = replTest.getNodeId(c_conn);

    // get master and do an initial write
    var master = replTest.getPrimary();
    var options = {writeConcern: {w: 2, wtimeout: 60000}};
    assert.writeOK(master.getDB(name).foo.insert({x: 1}, options));

    // add an oplog entry from the distant future as the most recent entry on node C
    var future_oplog_entry = conns[2].getDB("local").oplog.rs.find().sort({$natural: -1})[0];
    future_oplog_entry["ts"] = new Timestamp(future_oplog_entry["ts"].getTime() + 200000, 1);
    options = {writeConcern: {w: 1, wtimeout: 60000}};
    assert.writeOK(conns[2].getDB("local").oplog.rs.insert(future_oplog_entry, options));

    replTest.stop(CID);

    // We bump the term to make sure node 0's oplog is ahead of node 2's.
    var term = getLatestOp(conns[0]).t;
    try {
        assert.commandWorked(conns[0].adminCommand({replSetStepDown: 1, force: true}));
    } catch (e) {
        if (!isNetworkError(e)) {
            throw e;
        }
    }

    // After stepping down due to the higher term, it will eventually get reelected.
    replTest.waitForState(conns[0], ReplSetTest.State.PRIMARY);
    // Wait for the node to increase its term.
    assert.soon(function() {
        return getLatestOp(conns[0]).t > term;
    });

    // Node C should connect to new master as a sync source because chaining is disallowed.
    // C is ahead of master but it will still connect to it.
    clearRawMongoProgramOutput();
    replTest.restart(CID);

    assert.soon(function() {
        try {
            return rawMongoProgramOutput().match(
                "rollback error: not willing to roll back more than 30 minutes of data");
        } catch (e) {
            return false;
        }
    }, "node C failed to fassert", 60 * 1000);

    replTest.stopSet(undefined, undefined, {allowedExitCodes: [MongoRunner.EXIT_ABRUPT]});

}());
