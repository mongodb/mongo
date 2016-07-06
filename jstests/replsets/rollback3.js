/*
 * Basic test of a succesful replica set rollback for DDL operations.
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
        assert.eq(2, db.b.getIndexes().length);
        assert.eq(2, db.oldname.getIndexes().length);
        assert.eq(2, db.oldname.find().itcount());
        assert.eq(1, db.kap.find().itcount());
        assert(db.kap.isCapped());
        assert.eq(0, db.bar.count({q: 70}));
        assert.eq(33, db.bar.findOne({q: 0})["y"]);
        assert.eq(0, db.bar.count({q: 70}));
        assert.eq(1, db.bar.count({txt: "foo"}));
        assert.eq(200, db.bar.count({i: {$gt: -1}}));
        assert.eq(6, db.bar.count({q: {$gt: -1}}));
        assert.eq(0, db.getSiblingDB("abc").foo.find().itcount());
        assert.eq(0, db.getSiblingDB("abc").bar.find().itcount());
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
    replTest.waitForState(replTest.nodes[0], ReplSetTest.State.PRIMARY, 60 * 1000);
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
    assert.writeOK(a.b.insert({x: 1}));
    a.b.ensureIndex({x: 1});
    assert.writeOK(a.oldname.insert({y: 1}));
    assert.writeOK(a.oldname.insert({y: 2}));
    a.oldname.ensureIndex({y: 1}, true);
    assert.writeOK(a.bar.insert({q: 0}));
    assert.writeOK(a.bar.insert({q: 1, a: "foo"}));
    assert.writeOK(a.bar.insert({q: 2, a: "foo", x: 1}));
    assert.writeOK(a.bar.insert({q: 3, bb: 9, a: "foo"}));
    assert.writeOK(a.bar.insert({q: 40333333, a: 1}));
    for (var i = 0; i < 200; i++) {
        assert.writeOK(a.bar.insert({i: i}));
    }
    assert.writeOK(a.bar.insert({q: 40, a: 2}));
    assert.writeOK(a.bar.insert({q: 70, txt: 'willremove'}));
    a.createCollection("kap", {capped: true, size: 5000});
    assert.writeOK(a.kap.insert({foo: 1}));
    replTest.awaitReplication();

    // isolate A and wait for B to become master
    conns[0].disconnect(conns[1]);
    conns[0].disconnect(conns[2]);
    assert.soon(function() {
        try {
            return B.isMaster().ismaster;
        } catch (e) {
            return false;
        }
    });

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
    // drop a collection - we'll need all its data back!
    b.bar.drop();
    // drop an index - verify it comes back
    b.b.dropIndexes();
    // two to see if we transitively rollback?
    b.oldname.renameCollection("newname");
    b.newname.renameCollection("fooname");
    assert(b.fooname.find().itcount() > 0, "count rename");
    // test roll back (drop) a whole database
    var abc = b.getSisterDB("abc");
    assert.writeOK(abc.foo.insert({x: 1}));
    assert.writeOK(abc.bar.insert({y: 999}));

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
    assert(a.bar.find().itcount() >= 1, "count check");
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
