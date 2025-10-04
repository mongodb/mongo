// Tests that we can restart a replica set completely. Also tests that the config is saved properly
// between restarts.
//
// If all data-bearing nodes in a replica set are using an ephemeral storage engine, the set will
// not be able to survive a scenario where all data-bearing nodes are down simultaneously. In such a
// scenario, none of the members will have any replica set configuration document after a restart,
// so cannot elect a primary. This test induces such a scenario, so cannot be run on ephemeral
// storage engines.
// @tags: [requires_persistence]

import {ReplSetTest} from "jstests/libs/replsettest.js";

let compare_configs = function (c1, c2) {
    assert.eq(c1.version, c2.version, "version same");
    assert.eq(c1._id, c2._id, "_id same");

    for (let i in c1.members) {
        assert(c2.members[i] !== undefined, "field " + i + " exists in both configs");
        assert.eq(c1.members[i]._id, c2.members[i]._id, "id is equal in both configs");
        assert.eq(c1.members[i].host, c2.members[i].host, "host is equal in both configs");
    }
};

// Create a new replica set test. Specify set name and the number of nodes you want.
let replTest = new ReplSetTest({name: "testSet", nodes: 3});

// call startSet() to start each mongod in the replica set
// this returns a list of nodes
replTest.startSet();

// Call initiate() to send the replSetInitiate command
// This will wait for initiation
replTest.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

// Wait for at least one heartbeat to reach everyone, so that we will properly mark nodes as
// DOWN, later.
replTest.awaitSecondaryNodes();

// Call getPrimary to return a reference to the node that's been
// elected primary.
let primary = replTest.getPrimary();
let config1 = primary.getDB("local").system.replset.findOne();

// Now we're going to shut down all nodes
let pId = replTest.getNodeId(primary);
let [s1, s2] = replTest.getSecondaries();
let s1Id = replTest.getNodeId(s1);
let s2Id = replTest.getNodeId(s2);

replTest.stop(s1Id);
replTest.stop(s2Id);
replTest.waitForState(s1, ReplSetTest.State.DOWN);
replTest.waitForState(s2, ReplSetTest.State.DOWN);

replTest.stop(pId);

// Now let's restart these nodes
replTest.restart(pId);
replTest.restart(s1Id);
replTest.restart(s2Id);

// Make sure that a new primary comes up
primary = replTest.getPrimary();
replTest.awaitSecondaryNodes();
let config2 = primary.getDB("local").system.replset.findOne();
compare_configs(config1, config2);
replTest.stopSet();
