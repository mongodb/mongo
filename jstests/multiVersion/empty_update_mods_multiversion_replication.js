/**
 * Verifies that update commands with empty update modifiers do not produce oplog entries, and
 * therefore do not interfere with 4.4 - 4.7+ mixed-mode replication.
 *
 * TODO SERVER-50350: this test is specific to the 4.4 - 4.7+ upgrade process, and can be removed
 * when 5.0 becomes last-lts.
 */
(function() {
"use strict";

// Setup a two-node replica set - the primary is the latest version node and the secondary - 4.4.
const rst = new ReplSetTest({
    name: jsTestName(),
    nodes: [{binVersion: "latest"}, {binVersion: "last-lts", rsConfig: {priority: 0}}]
});
rst.startSet();
rst.initiate();
const testDB = rst.getPrimary().getDB("test");
const coll = testDB[jsTestName()];

// Insert a test document.
assert.commandWorked(coll.insert({_id: 1, a: 1}));

// Issue update commands with empty update modifiers on the latest version node.
const updateModifiers = [
    "$set",
    "$unset",
    "$inc",
    "$mul",
    "$push",
    "$addToSet",
    "$pull",
    "$rename",
    "$bit",
    "$max",
    "$min",
    "$currentDate",
    "$setOnInsert",
    "$pop",
    "$pullAll"
];
for (const modifier of updateModifiers) {
    assert.commandWorked(coll.update({_id: 1}, {[modifier]: {}}));
}

// Verify that the oplog does not have any entries for the update commands with empty update
// modifiers.
const oplogMatches = rst.findOplog(rst.getPrimary(), {ns: coll.getFullName()}, 10).itcount();
assert.eq(1,
          oplogMatches,
          rst.findOplog(rst.getPrimary(),
                        {},
                        10)
              .toArray());  // Only insert record is found.

// Issue an update command that modifies the document.
assert.commandWorked(coll.update({_id: 1}, {$set: {a: 2}}));

// Verify that the update command was propagated to the secondary node.
rst.awaitReplication();
assert.docEq(coll.find().toArray(),
             rst.getSecondary().getCollection(coll.getFullName()).find().toArray());

rst.stopSet();
})();