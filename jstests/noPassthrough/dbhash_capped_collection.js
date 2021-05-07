/**
 * Tests that the dbHash command separately lists the names of capped collections on the database.
 *
 * @tags: [requires_replication, requires_capped]
 */
(function() {
"use strict";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const db = rst.getPrimary().getDB("test");

// We create a capped collection as well as a non-capped collection and verify that the "capped"
// field in the dbHash command response only lists the capped one.
assert.commandWorked(db.runCommand({create: "noncapped"}));
assert.commandWorked(db.runCommand({create: "capped", capped: true, size: 4096}));

let res = assert.commandWorked(db.runCommand({dbHash: 1}));
assert.eq(["capped", "noncapped"], Object.keys(res.collections).sort());
assert.eq(["capped"], res.capped);

// If the capped collection is excluded from the list of collections to md5sum, then it won't
// appear in the "capped" field either.
res = assert.commandWorked(db.runCommand({dbHash: 1, collections: ["noncapped"]}));
assert.eq([], res.capped);

rst.stopSet();
})();
