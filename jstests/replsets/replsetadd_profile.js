// Tests adding node to replica set with profiling enabled.
// Verifies that the oplog replay hack is compatible with
// the profiling option.
// One of the ways to exercise the oplog replay hack is to
// add a new node to an existing active replica set.

import {ReplSetTest} from "jstests/libs/replsettest.js";

// Initialize a single node replica set where
// the only node is running at a profiling level of 2.
let collectionName = "jstests_replsetadd_profile";

let replTest = new ReplSetTest({name: "ReplSetAddProfileTestSet", nodes: [{profile: 2}]});
replTest.startSet();
replTest.initiate();
let primary = replTest.getPrimary();
let primaryCollection = primary.getDB("test").getCollection(collectionName);
primaryCollection.save({a: 1});

// Add a new node with no profiling level.
let newNode = replTest.add();
replTest.reInitiate();

replTest.awaitSecondaryNodes(null, [replTest.nodes[1]]);
// Allow documents to propagate to new replica set member.
replTest.awaitReplication();

let newNodeCollection = newNode.getDB("test").getCollection(collectionName);
assert.eq(1, newNodeCollection.find({a: 1}).itcount(), "expect documents to be present in secondary after replication");

let signal = 15;
replTest.stopSet(signal);
