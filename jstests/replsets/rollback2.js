/*
 * Basic test of a succesful replica set rollback for CRUD operations.
 *
 * This tests sets up a 3 node set, data-bearing nodes A and B and an arbiter.
 *
 * 1. A is elected PRIMARY and receives several writes, which are propagated to B.
 * 2. A is isolated from the rest of the set and B is elected PRIMARY.
 * 3. B receives several operations, which will later be undone during rollback.
 * 4. B is then isolated and A regains its connection to the arbiter.
 * 5. A receives many new operations, which B will replicate after rollback.
 * 6. B rejoins the set and goes through the rollback process.
 * 7. The contents of A and B are compare to ensure the rollback results in consistent nodes.
 */
load("jstests/replsets/rslib.js");

(function() {
    "use strict";
    // helper function for verifying contents at the end of the test
    var checkFinalResults = function(db) {
        assert.eq(0, db.bar.count({q: 70}));
        assert.eq(2, db.bar.count({q: 40}));
        assert.eq(3, db.bar.count({a: "foo"}));
        assert.eq(6, db.bar.count({q: {$gt: -1}}));
        assert.eq(1, db.bar.count({txt: "foo"}));
        assert.eq(33, db.bar.findOne({q: 0})["y"]);
        assert.eq(1, db.kap.find().itcount());
        assert.eq(0, db.kap2.find().itcount());
    };

    var name = "rollback2js";
    var replTest = new ReplSetTest({name: name, nodes: 3, useBridge: true});
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

    // Make sure we have a master and that that master is node A
    replTest.waitForState(replTest.nodes[0], ReplSetTest.State.PRIMARY);
    var master = replTest.getPrimary();
    var a_conn = conns[0];
    a_conn.setSlaveOk();
    var A = a_conn.getDB("admin");
    var b_conn = conns[1];
    b_conn.setSlaveOk();
    var B = b_conn.getDB("admin");
    assert.eq(master, conns[0], "conns[0] assumed to be master");
    assert.eq(a_conn, master);

    // Wait for initial replication
    var a = a_conn.getDB("foo");
    var b = b_conn.getDB("foo");

    // initial data for both nodes
    assert.writeOK(a.bar.insert({q: 0}));
    assert.writeOK(a.bar.insert({q: 1, a: "foo"}));
    assert.writeOK(a.bar.insert({q: 2, a: "foo", x: 1}));
    assert.writeOK(a.bar.insert({q: 3, bb: 9, a: "foo"}));
    assert.writeOK(a.bar.insert({q: 40, a: 1}));
    assert.writeOK(a.bar.insert({q: 40, a: 2}));
    assert.writeOK(a.bar.insert({q: 70, txt: 'willremove'}));
    a.createCollection("kap", {capped: true, size: 5000});
    assert.writeOK(a.kap.insert({foo: 1}));
    // going back to empty on capped is a special case and must be tested
    a.createCollection("kap2", {capped: true, size: 5501});
    replTest.awaitReplication();

    var timeout;
    if (replTest.getReplSetConfigFromNode().protocolVersion == 1) {
        timeout = 30 * 1000;
    } else {
        timeout = 60 * 1000;
    }
    // isolate A and wait for B to become master
    conns[0].disconnect(conns[1]);
    conns[0].disconnect(conns[2]);
    assert.soon(function() {
        try {
            return B.isMaster().ismaster;
        } catch (e) {
            return false;
        }
    }, "node B did not become master as expected", timeout);

    // do operations on B and B alone, these will be rolled back
    assert.writeOK(b.bar.insert({q: 4}));
    assert.writeOK(b.bar.update({q: 3}, {q: 3, rb: true}));
    assert.writeOK(b.bar.remove({q: 40}));  // multi remove test
    assert.writeOK(b.bar.update({q: 2}, {q: 39, rb: true}));
    // rolling back a delete will involve reinserting the item(s)
    assert.writeOK(b.bar.remove({q: 1}));
    assert.writeOK(b.bar.update({q: 0}, {$inc: {y: 1}}));
    assert.writeOK(b.kap.insert({foo: 2}));
    assert.writeOK(b.kap2.insert({foo: 2}));
    // create a collection (need to roll back the whole thing)
    assert.writeOK(b.newcoll.insert({a: true}));
    // create a new empty collection (need to roll back the whole thing)
    b.createCollection("abc");

    // isolate B, bring A back into contact with the arbiter, then wait for A to become master
    // insert new data into A so that B will need to rollback when it reconnects to A
    conns[1].disconnect(conns[2]);
    assert.soon(function() {
        try {
            return !B.isMaster().ismaster;
        } catch (e) {
            return false;
        }
    });

    conns[0].reconnect(conns[2]);
    assert.soon(function() {
        try {
            return A.isMaster().ismaster;
        } catch (e) {
            return false;
        }
    });
    assert.gte(a.bar.find().itcount(), 1, "count check");
    assert.writeOK(a.bar.insert({txt: 'foo'}));
    assert.writeOK(a.bar.remove({q: 70}));
    assert.writeOK(a.bar.update({q: 0}, {$inc: {y: 33}}));

    // A is 1 2 3 7 8
    // B is 1 2 3 4 5 6
    // put B back in contact with A and arbiter, as A is primary, B will rollback and then catch up
    conns[1].reconnect(conns[2]);
    conns[0].reconnect(conns[1]);

    awaitOpTime(b.getMongo(), getLatestOp(a_conn).ts);

    // await steady state and ensure the two nodes have the same contents
    replTest.awaitSecondaryNodes();
    replTest.awaitReplication();
    checkFinalResults(a);
    checkFinalResults(b);

    replTest.stopSet(15);
}());
