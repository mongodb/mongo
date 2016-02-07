/*
 * Basic test of successful rollback in replica sets.
 *
 * This test sets up a 3-node set, with an arbiter and 2 data-bearing nodes, A and B.
 * A is the initial primary node.
 *
 * The test inserts 3 documents into A, and waits for them to replicate to B.  Then, it partitions A
 * from the other nodes, causing it to step down and causing B to be elected primary.
 *
 * Next, 3 more documents inserted into B, and B is partitioned from the arbiter.
 *
 * Next, A is allowed to connect to the arbiter again, and gets reelected primary.  Because the
 * arbiter doesn't know about the writes that B accepted, A becomes primary and we insert 3 new
 * documents.  Now, A and B have diverged.  We heal the remaining network partition, bringing B back
 * into the network.
 *
 * Finally, we expect either A or B to roll back its 3 divergent documents and acquire the other
 * node's.
 */
load("jstests/replsets/rslib.js");

(function () {
    "use strict";
    // helper function for verifying contents at the end of the test
    var checkFinalResults = function(db) {
        var x = db.bar.find().sort({q: 1}).toArray();
        assert.eq(5, x.length, "incorrect number of documents found. Docs found: " + tojson(x));
        assert.eq(1, x[0].q);
        assert.eq(2, x[1].q);
        assert.eq(3, x[2].q);
        assert.eq(7, x[3].q);
        assert.eq(8, x[4].q);
    };

    var replTest = new ReplSetTest({ name: 'unicomplex', nodes: 3, oplogSize: 1, useBridge: true });
    var nodes = replTest.nodeList();

    var conns = replTest.startSet();
    var r = replTest.initiate({ "_id": "unicomplex",
        "members": [
                             { "_id": 0, "host": nodes[0], "priority": 3 },
                             { "_id": 1, "host": nodes[1] },
                             { "_id": 2, "host": nodes[2], arbiterOnly: true}]
    });

    // Make sure we have a master
    replTest.waitForState(replTest.nodes[0], ReplSetTest.State.PRIMARY, 60 * 1000);
    var master = replTest.getPrimary();
    var a_conn = conns[0];
    var A = a_conn.getDB("admin");
    var b_conn = conns[1];
    a_conn.setSlaveOk();
    b_conn.setSlaveOk();
    var B = b_conn.getDB("admin");
    assert(master == conns[0], "conns[0] assumed to be master");
    assert(a_conn == master);

    // Wait for initial replication
    var a = a_conn.getDB("foo");
    var b = b_conn.getDB("foo");

    /* force the oplog to roll */
    if (new Date() % 2 == 0) {
        jsTest.log("ROLLING OPLOG AS PART OF TEST (we only do this sometimes)");
        var pass = 1;
        var first = a.getSisterDB("local").oplog.rs.find().sort({ $natural: 1 }).limit(1)[0];
        a.roll.insert({ x: 1 });
        while (1) {
            var bulk = a.roll.initializeUnorderedBulkOp();
            for (var i = 0; i < 1000; i++) {
                bulk.find({}).update({ $inc: { x: 1 }});
            }
            // unlikely secondary isn't keeping up, but let's avoid possible intermittent 
            // issues with that.
            assert.writeOK(bulk.execute({ w: 2 }));

            var op = a.getSisterDB("local").oplog.rs.find().sort({ $natural: 1 }).limit(1)[0];
            if (tojson(op.h) != tojson(first.h)) {
                printjson(op);
                printjson(first);
                break;
            }
            pass++;
        }
        jsTest.log("PASSES FOR OPLOG ROLL: " + pass);
    }
    else {
        jsTest.log("NO ROLL");
    }

    assert.writeOK(a.bar.insert({ q: 1, a: "foo" }));
    assert.writeOK(a.bar.insert({ q: 2, a: "foo", x: 1 }));
    assert.writeOK(a.bar.insert({ q: 3, bb: 9, a: "foo" }, { writeConcern: { w: 2 } }));

    assert.eq(a.bar.count(), 3, "a.count");
    assert.eq(b.bar.count(), 3, "b.count");

    conns[0].disconnect(conns[1]);
    conns[0].disconnect(conns[2]);
    replTest.waitForState(b.getMongo(), ReplSetTest.State.PRIMARY, 60 * 1000);

    // These 97 documents will be rolled back eventually.
    for (var i = 4; i <= 100; i++) {
        assert.writeOK(b.bar.insert({ q: i }));
    }
    assert.eq(100, b.bar.count(), "u.count");

    // a should not have the new data as it was partitioned.
    conns[1].disconnect(conns[2]);
    jsTest.log("*************** wait for server to reconnect ****************");
    conns[0].reconnect(conns[2]);

    jsTest.log("*************** B ****************");
    assert.soon(function () { try { return !B.isMaster().ismaster; } catch(e) { return false; } });
    jsTest.log("*************** A ****************");
    assert.soon(function () { try { return A.isMaster().ismaster; } catch(e) { return false; } });

    assert(a.bar.count() == 3, "t is 3");
    assert.writeOK(a.bar.insert({ q: 7 }));
    assert.writeOK(a.bar.insert({ q: 8 }));

    // A is 1 2 3 7 8
    // B is 1 2 3 4 5 6 ... 100

    var connectionsCreatedOnPrimaryBeforeRollback = a.serverStatus().connections.totalCreated;
    // bring B back online
    conns[0].reconnect(conns[1]);
    conns[1].reconnect(conns[2]);

    awaitOpTime(b.getMongo(), getLatestOp(a_conn).ts);
    replTest.awaitSecondaryNodes();
    replTest.awaitReplication();
    checkFinalResults(a);
    checkFinalResults(b);

    var connectionsCreatedOnPrimaryAfterRollback = a.serverStatus().connections.totalCreated;
    var connectionsCreatedOnPrimaryDuringRollback =
        connectionsCreatedOnPrimaryAfterRollback - connectionsCreatedOnPrimaryBeforeRollback;
    jsTest.log('connections created during rollback = ' + connectionsCreatedOnPrimaryDuringRollback);
    assert.lt(connectionsCreatedOnPrimaryDuringRollback, 50,
              'excessive number of connections made by secondary to primary during rollback');

    replTest.stopSet(15);
}());
