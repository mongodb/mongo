// Tests adding node to replica set with profiling enabled.
// Verifies that the oplog replay hack is compatible with
// the profiling option.
// One of the ways to exercise the oplog replay hack is to
// add a new node to an existing active replica set.

// Initialize a single node replica set where
// the only node is running at a profiling level of 2.
var collectionName = 'jstests_replsetadd_profile';

var replTest = new ReplSetTest({name: 'ReplSetAddProfileTestSet', nodes: [{profile: 2}]});
replTest.startSet();
replTest.initiate();
var master = replTest.getPrimary();
var masterCollection = master.getDB('test').getCollection(collectionName);
masterCollection.save({a: 1});

// Add a new node with no profiling level.
var newNode = replTest.add();
replTest.reInitiate();

replTest.waitForState(replTest.nodes[1], ReplSetTest.State.SECONDARY);
// Allow documents to propagate to new replica set member.
replTest.awaitReplication();

var newNodeCollection = newNode.getDB('test').getCollection(collectionName);
assert.eq(1,
          newNodeCollection.find({a: 1}).itcount(),
          'expect documents to be present in slave after replication');

var signal = 15;
replTest.stopSet(signal);
