/**
 * Test initial sync with views present.
 */

(function() {
    "use strict";

    load("jstests/replsets/rslib.js");
    let testName = "initial_sync_views";
    let hostName = getHostName();

    let replTest = new ReplSetTest({name: testName, nodes: 1});
    replTest.startSet();
    replTest.initiate();

    let primaryDB = replTest.getPrimary().getDB(testName);

    for (let i = 0; i < 10; ++i) {
        assert.writeOK(primaryDB.coll.insert({a: i}));
    }

    // Setup view.
    assert.commandWorked(
        primaryDB.runCommand({create: "view", viewOn: "coll", pipeline: [{$match: {a: 5}}]}));

    assert.eq(10, primaryDB.coll.find().itcount());
    assert.eq(1, primaryDB.view.find().itcount());

    // Add new member to the replica set and wait for initial sync to complete.
    let secondary = replTest.add();
    replTest.reInitiate();
    replTest.awaitReplication();
    replTest.awaitSecondaryNodes();

    // Confirm secondary has expected collection and view document count.
    let secondaryDB = secondary.getDB(testName);
    assert.eq(10, secondaryDB.coll.find().itcount());
    assert.eq(1, secondaryDB.view.find().itcount());

    replTest.stopSet();
})();
