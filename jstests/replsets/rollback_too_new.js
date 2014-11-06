// test that a rollback of an op more than 1800 secs newer than the new master causes fatal shutdown

(function() {
    "use strict";
    // set up a set and grab things for later
    var name = "rollback_too_new";
    var replTest = new ReplSetTest({name: name, nodes: 3});
    var nodes = replTest.nodeList();
    var conns = replTest.startSet();
    replTest.initiate({"_id": name,
                       "members": [
                           { "_id": 0, "host": nodes[0], priority: 3 },
                           { "_id": 1, "host": nodes[1] },
                           { "_id": 2, "host": nodes[2], arbiterOnly: true}],
                       "settings": {
                           "chainingAllowed": false,
                       }
                      });
    var a_conn = conns[0];
    var b_conn = conns[1];
    var AID = replTest.getNodeId(a_conn);
    var BID = replTest.getNodeId(b_conn);

    // get master and do an initial write
    var master = replTest.getMaster();
    assert.eq(master, conns[0], "conns[0] assumed to be master");
    assert.eq(a_conn.host, master.host, "a_conn assumed to be master");
    var options = {writeConcern: {w: 2, wtimeout: 60000}, upsert: true};
    assert.writeOK(a_conn.getDB(name).foo.insert({x: 1}, options));

    // remove node B from the set
    replTest.stop(BID);

    // add an oplog entry from the distant future as the most recent entry on node A
    var future_oplog_entry = master.getDB("local").oplog.rs.find().sort({$natural: -1})[0];
    future_oplog_entry["ts"] = new Timestamp(future_oplog_entry["ts"].getTime() + 200000, 1);
    var options = {writeConcern: {w: 1, wtimeout: 60000}, upsert: true};
    assert.writeOK(master.getDB("local").oplog.rs.insert(future_oplog_entry, options));

    // take down node A, allow node B to become master and do one write to it
    // in order to force a rollback on A
    replTest.stop(AID);
    replTest.restart(BID);
    master = replTest.getMaster();
    assert.eq(b_conn.host, master.host, "b_conn assumed to be master");
    assert.writeOK(master.getDB(name).foo.insert({x: 2}, options));

    // restart node A, which should attempt to rollback but then fassert.
    clearRawMongoProgramOutput();
    replTest.restart(AID);
    assert.soon(function() {
        try {
            return rawMongoProgramOutput().match(
                "rollback error: not willing to roll back more than 30 minutes of data");
        } catch (e) {
            return false;
        }
    }, "A failed to fassert", 60 * 1000);

    replTest.stopSet();

}());
