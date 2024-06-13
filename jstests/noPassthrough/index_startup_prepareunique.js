/**
 * Checks that a prepareUnique index does not block startup.
 *
 * @tags: [requires_persistence, requires_replication]
 */
(function() {
'use strict';

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
assert.commandWorked(primary.getDB("admin").system.version.createIndex({"version": 1}));
assert.commandWorked(primary.getDB("admin").runCommand(
    {collMod: "system.version", index: {keyPattern: {version: 1}, prepareUnique: true}}));

// Restarting the node after prepareUnique: true should succeed.
rst.restart(primary);
rst.waitForPrimary();
primary = rst.getPrimary();

rst.stopSet();
})();
