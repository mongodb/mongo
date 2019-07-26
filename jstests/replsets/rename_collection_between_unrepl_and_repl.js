/**
 * Tests that renameCollection disallows renaming between an unreplicated and a replicated
 * namespace in both directions. Unreplicated collections are unique to the nodes that own
 * them, so allowing such renames would introduce consistency risks. This test uses the
 * 'local' database for unreplicated namespaces.
 */

(function() {
"use strict";

const name = "rename_collection_between_unrepl_and_repl";
const rst = new ReplSetTest({"name": name, "nodes": 1});
rst.startSet();
rst.initiate();
const primary = rst.getPrimary();

/**
 * Part 1: Attempt to rename from a replicated to an unreplicated namespace.
 */
let sourceNs = "somedb.replicated";
let targetNs = "local.unreplicated";

// Ensure that the source collection exists.
assert.commandWorked(primary.getCollection(sourceNs).insert({"fromRepl": "toUnrepl"}));

assert.commandFailedWithCode(primary.adminCommand({"renameCollection": sourceNs, "to": targetNs}),
                             ErrorCodes.IllegalOperation);

/**
 * Part 2: Attempt to rename from an unreplicated to a replicated namespace.
 */
sourceNs = "local.alsoUnreplicated";
targetNs = "somedb.alsoReplicated";

// Ensure that the source collection exists.
assert.commandWorked(primary.getCollection(sourceNs).insert({"fromUnrepl": "toRepl"}));

assert.commandFailedWithCode(primary.adminCommand({"renameCollection": sourceNs, "to": targetNs}),
                             ErrorCodes.IllegalOperation);

rst.stopSet();
})();
