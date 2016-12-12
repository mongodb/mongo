/*
 * Basic test of successful replica set rollback for system.views creation.
 *
 * This test sets up a 3 node set, data-bearing nodes A and B and an arbiter.
 *
 * 1. A is elected PRIMARY and inserts into "test1.coll", which is propagated to B.
 * 2. A is isolated from the rest of the set and B is elected PRIMARY.
 * 3. B creates views "test1.x" and "test2.y" (creating test[12].system.views)
 *    and collection "test3.z", which will later be undone during rollback.
 * 4. B is then isolated and A regains its connection to the arbiter.
 * 5. A inserts a document into collection "test1.x", and creates views "test2.y" and "test3.z"
 *     which B will replicate after rollback.
 * 6. B rejoins the set and goes through the rollback/recovery process.
 * 7. The contents of A and B are compared to ensure the rollback results in consistent nodes,
 *    and have the expected collections and views..
 */
load("jstests/replsets/rslib.js");

(function() {
    "use strict";

    // Run a command, return the result if it worked, or assert with a message otherwise.
    let checkedRunCommand = (db, cmd) =>
        ((res, msg) => (assert.commandWorked(res, msg), res))(db.runCommand(cmd), tojson(cmd));

    // Like db.getCollectionNames, but allows a filter and works without system.namespaces.
    let getCollectionNames = (db, filter) => checkedRunCommand(db, {listCollections: 1, filter})
                                                 .cursor.firstBatch.map((entry) => entry.name)
                                                 .sort()
                                                 .filter((name) => name != "system.indexes");

    // Function that checks that all array elements are equal, and returns the unique element.
    let checkEqual = (array, what) =>
        array.reduce((x, y) => assert.eq(x, y, "nodes don't have matching " + what) || x);

    // Helper function for verifying database contents at the end of the test.
    let checkFinalResults = (dbs, expectedColls, expectedViews) => ({
        dbname: checkEqual(dbs, "names"),
        colls: checkEqual(
            dbs.map((db) => getCollectionNames(db, {type: "collection"})).concat([expectedColls]),
            "colls"),
        views: checkEqual(
            dbs.map((db) => getCollectionNames(db, {type: "view"})).concat([expectedViews]),
            "views"),
        md5: checkEqual(dbs.map((db) => checkedRunCommand(db, {dbHash: 1}).md5), "hashes")
    });

    let name = "rollback_views.js";
    let replTest = new ReplSetTest({name: name, nodes: 3, useBridge: true});
    let nodes = replTest.nodeList();

    let conns = replTest.startSet();
    replTest.initiate({
        "_id": name,
        "members": [
            {"_id": 0, "host": nodes[0], priority: 3},
            {"_id": 1, "host": nodes[1]},
            {"_id": 2, "host": nodes[2], arbiterOnly: true}
        ]
    });

    // Make sure we have a primary and that that primary is node A.
    replTest.waitForState(replTest.nodes[0], ReplSetTest.State.PRIMARY);

    let nodeA = conns[0];
    let nodeB = conns[1];
    let arbiter = conns[2];

    let a1 = nodeA.getDB("test1");
    let b1 = nodeB.getDB("test1");

    // Initial data for both nodes.
    assert.writeOK(a1.coll.insert([{_id: 1, x: 1}, {_id: 2, x: 2}]));

    // Wait for initial replication.
    replTest.awaitReplication();

    // Isolate A and wait for B to become primary.
    nodeA.disconnect(nodeB);
    nodeA.disconnect(arbiter);
    assert.soon(() => replTest.getPrimary() == nodeB, "node B did not become primary as expected");

    // Do operations on B and B alone, these will be rolled back.
    // For the collection creation, first create a view with the same name, stressing rollback.
    assert.writeOK(b1.coll.remove({x: 2}));
    assert.commandWorked(b1.createView("x", "coll", [{$match: {x: 1}}]));
    let b2 = b1.getSiblingDB("test2");
    assert.writeOK(b2.coll.insert([{_id: 1, y: 1}, {_id: 2, y: 2}]));
    assert.commandWorked(b2.createView("y", "coll", [{$match: {y: 2}}]));
    let b3 = b1.getSiblingDB("test3");
    assert.commandWorked(b3.createView("z", "coll", []));
    assert.writeOK(b3.system.views.remove({}));
    assert.writeOK(b3.z.insert([{z: 1}, {z: 2}, {z: 3}]));
    assert.writeOK(b3.z.remove({z: 1}));

    // Isolate B, bring A back into contact with the arbiter, then wait for A to become primary.
    // Insert new data into A, so that B will need to rollback when it reconnects to A.
    nodeB.disconnect(arbiter);
    replTest.awaitNoPrimary();
    nodeA.reconnect(arbiter);
    assert.soon(() => replTest.getPrimary() == nodeA, "nodeA did not become primary as expected");

    // A is now primary and will perform writes that must be copied by B after rollback.
    assert.eq(a1.coll.find().itcount(), 2, "expected two documents in test1.coll");
    assert.writeOK(a1.x.insert({_id: 3, x: "string in test1.x"}));
    let a2 = a1.getSiblingDB("test2");
    assert.commandWorked(a2.createView("y", "coll", [{$match: {y: 2}}]));
    assert.writeOK(a2.coll.insert([{_id: 1, y: 1}, {_id: 2, y: 2}]));
    let a3 = a1.getSiblingDB("test3");
    assert.writeOK(a3.coll.insert([{z: 1}, {z: 2}, {z: 3}]));
    assert.commandWorked(a3.createView("z", "coll", [{$match: {z: 3}}]));

    // A is collections: test1.{coll,x}, test2.{coll,system.views}, test3.{coll,system.views}
    //            views: test2.y, test3.z
    // B is collections: test1.{coll,system.views}, test2.{coll,systems}, test3.{z,system.views}
    //            views: test1.x, test2.y
    //
    // Put B back in contact with A and arbiter. A is primary, so B will rollback and catch up.
    nodeB.reconnect(arbiter);
    nodeA.reconnect(nodeB);

    awaitOpTime(b1.getMongo(), getLatestOp(nodeA).ts);

    // Await steady state and ensure the two nodes have the same contents.
    replTest.awaitSecondaryNodes();
    replTest.awaitReplication();

    // Check both nodes agree with each other and with the expected set of views and collections.
    print("All done, check that both nodes have the expected collections, views and md5.");
    printjson(checkFinalResults([a1, b1], ["coll", "x"], []));
    printjson(checkFinalResults([a2, b2], ["coll", "system.views"], ["y"]));
    printjson(checkFinalResults([a3, b3], ["coll", "system.views"], ["z"]));

    replTest.stopSet();
}());
