// Create a replica set with feature compatibility version set to 3.4, add a
// binVersion 3.4 member, and then update documents with each member as the
// primary. Finally, upgrade the 3.4 member to 3.6, upgrade the replica set to
// feature compatibility version 3.6, and again update documents with each
// member as the primary.

const testName = "updates_in_heterogeneous_repl_set";

(function() {
    "use strict";

    // Initialize the binVersion 3.6 versions of the replica set.
    let replTest =
        new ReplSetTest({name: testName, nodes: [{binVersion: "latest"}, {binVersion: "latest"}]});

    replTest.startSet();
    replTest.initiate();

    let primary = replTest.getPrimary();

    // Set the feature compatibility version to 3.4.
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: "3.4"}));

    // Add the binVersion 3.4 member fo the replica set.
    let binVersion34Node = replTest.add({binVersion: "3.4"});
    replTest.reInitiate();
    replTest.awaitSecondaryNodes();

    // Give each member a chance to be primary while updating documents.
    let collIndex = 0;
    replTest.nodes.forEach(function(node) {
        replTest.stepUp(node);

        let coll = node.getDB("test")["coll" + (collIndex++)];

        for (let id = 0; id < 1000; id++) {
            assert.writeOK(coll.insert({_id: id}));
            assert.writeOK(coll.update({_id: id}, {$set: {z: 1, a: 2}}));

            // Because we are using the update system from earlier MongodDB
            // versions (as a result of using feature compatibility version
            // 3.4), we expect to see the new 'z' and 'a' fields to get added in
            // the same order as they appeared in the update document.
            assert.eq(coll.findOne({_id: id}), {_id: id, z: 1, a: 2});
        }
    });

    // Upgrade the binVersion 3.4 member to binVersion 3.6.
    replTest.restart(binVersion34Node, {binVersion: "latest"});
    replTest.awaitSecondaryNodes();

    // Set the replica set feature compatibility version to 3.6.
    primary = replTest.getPrimary();
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: "3.6"}));

    // Again, give each member a chance to be primary while updating documents.
    replTest.nodes.forEach(function(node) {
        replTest.stepUp(node);

        let coll = node.getDB("test")["coll" + (collIndex++)];

        for (let id = 0; id < 1000; id++) {
            assert.writeOK(coll.insert({_id: id}));
            assert.writeOK(coll.update({_id: id}, {$set: {z: 1, a: 2}}));

            // We can tell that we are using the new 3.6 update system, because
            // it inserts new fields in lexicographic order, causing the 'a' and
            // 'z' fields to be swapped.
            assert.eq(coll.findOne({_id: id}), {_id: id, a: 2, z: 1});
        }
    });

    replTest.stopSet();
})();
