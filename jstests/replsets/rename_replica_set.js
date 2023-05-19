/**
 * Test the procedure for renaming a replica set.
 *
 * 1. Restart each node in the set in standalone mode.
 * 2. Replace each node's config doc with a doc that contains the new name.
 * 3. Restart the node as part of the newly named replica set.
 *
 * @tags: [requires_persistence]
 */

(function() {
"use strict";

const replTest = new ReplSetTest({name: 'testSet', nodes: 2});
let nodes = replTest.startSet();
replTest.initiate();

let primary = replTest.getPrimary();

const dbName = "test";
const collName = jsTestName();
let coll = primary.getDB(dbName)[collName];
const newReplSetName = "newTestSet";

// Make sure a write can be replicated to every node in the set. We set journaling to true to ensure
// this write is durable before restarting the nodes below.
assert.commandWorked(coll.insert({a: 1}, {"writeConcern": {"w": 2, "j": true}}));

// Restart all nodes in the set as standalones.
nodes = replTest.restart(nodes, {noReplSet: true});

// Change each node's config to have the new replica set name.
replTest.nodes.forEach(function(node) {
    let replsetColl = node.getDB("local").system.replset;
    let doc = replsetColl.findOne();

    let oldName = doc._id;
    doc._id = newReplSetName;

    // We can't update the doc since the replica set is the _id, so insert a new config and remove
    // the old one.
    replsetColl.insert(doc);
    replsetColl.remove({_id: oldName});

    // Restart the node now as part of the newly named replica set.
    replTest.restart(node, {replSet: newReplSetName});
});

// The nodes will only be able to elect a new primary if both nodes have the same replSet name.
primary = replTest.getPrimary();
let secondary = replTest.getSecondary();
coll = primary.getDB(dbName)[collName];

let primaryReplSetName = primary.getDB("local").system.replset.findOne()._id;
let secondaryReplSetName = secondary.getDB("local").system.replset.findOne()._id;
assert.eq(primaryReplSetName, newReplSetName);
assert.eq(secondaryReplSetName, newReplSetName);

// Make sure a write can still be replicated to every node in the set after the rename.
assert.commandWorked(coll.insert({b: 2}, {"writeConcern": {"w": 2}}));

replTest.stopSet();
}());
