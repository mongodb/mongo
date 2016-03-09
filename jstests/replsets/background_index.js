/** Tests that a background index will be successfully
 *  replicated to a secondary when the indexed collection
 *  is renamed.
 */

(function() {
    "use strict";

    // Bring up a 2 node replset.
    var name = "bg_index_rename";
    var rst = new ReplSetTest({name: name, nodes: 3});
    rst.startSet();
    rst.initiate();

    // Create and populate a collection.
    var primary = rst.getPrimary();
    var coll = primary.getCollection("test.foo");
    var adminDB = primary.getDB("admin");

    for (var i = 0; i < 100; i++) {
        assert.writeOK(coll.insert({_id: i, x: i * 3, str: "hello world"}));
    }

    // Add a background index.
    coll.ensureIndex({x: 1}, {background: true});

    // Rename the collection.
    assert.commandWorked(
        adminDB.runCommand({renameCollection: "test.foo", to: "bar.test", dropTarget: true}),
        "Call to renameCollection failed.");

    // Await replication.
    rst.awaitReplication();

    // Step down the primary.
    try {
        adminDB.runCommand({replSetStepDown: 60, force: true});
    } catch (e) {
        // Left empty on purpose.
    }

    // Wait for new primary.
    var newPrimary = rst.getPrimary();
    assert.neq(primary, newPrimary);
    var barDB = newPrimary.getDB("bar");
    coll = newPrimary.getCollection("bar.test");
    coll.insert({_id: 200, x: 600, str: "goodnight moon"});

    // Check that the new primary has the index
    // on the renamed collection.
    var indexes = barDB.runCommand({listIndexes: "test"});
    assert.eq(indexes.cursor.firstBatch.length, 2);

    rst.stopSet();
}());
