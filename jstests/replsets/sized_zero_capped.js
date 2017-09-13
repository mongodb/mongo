// Ensure replicating a createCollection command with capped:true and size:0 does not cause a
// SECONDARY to crash. (see SERVER-18792)

(function() {
    "use strict";

    var name = "sized_zero_capped";
    var replTest = new ReplSetTest({name: name, nodes: 3});
    var nodes = replTest.nodeList();
    replTest.startSet();
    replTest.initiate({
        "_id": name,
        "members": [
            {"_id": 0, "host": nodes[0], priority: 3},
            {"_id": 1, "host": nodes[1], priority: 0},
            {"_id": 2, "host": nodes[2], priority: 0}
        ]
    });

    var testDB = replTest.getPrimary().getDB(name);
    testDB.createCollection(name, {capped: true, size: 0});
    replTest.awaitReplication();

    // ensure secondary is still up and responsive
    var secondary = replTest.getSecondary();
    assert.commandWorked(secondary.getDB(name).runCommand({ping: 1}));

    replTest.stopSet();
}());
