/**
 * Tests that user can not downgrade to lastLTS FCV if there exists a collection with conflicting
 * indexes.
 */
(function() {
"use strict";

const rst = new ReplSetTest({nodes: [{binVersion: "latest"}, {binVersion: "latest"}]});
rst.startSet();
rst.initiate();

const db = rst.getPrimary().getDB("test");
const coll = db.getCollection("coll");

// Create multiple indexes on a collection with conflicting options.
assert.commandWorked(coll.runCommand("createIndexes", {
    indexes: [
        {key: {x: 1}, name: "x1", sparse: true},
        {key: {x: 1}, name: "x2", unique: true},
    ]
}));

// Ensure that downgrade to the lastLTS is failing.
assert.commandFailedWithCode(db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}),
                             ErrorCodes.CannotDowngrade);

rst.stopSet();
})();
