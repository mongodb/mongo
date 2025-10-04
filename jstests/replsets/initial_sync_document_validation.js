/**
 * Tests that initial sync does not fail if it inserts documents which don't validate.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

let name = "initial_sync_document_validation";
let replSet = new ReplSetTest({
    name: name,
    nodes: 2,
});

replSet.startSet();
replSet.initiate();
let primary = replSet.getPrimary();
let secondary = replSet.getSecondary();

let coll = primary.getDB("test").getCollection(name);
assert.commandWorked(coll.insert({_id: 0, x: 1}));
assert.commandWorked(coll.runCommand("collMod", {"validator": {a: {$exists: true}}}));

secondary = replSet.restart(secondary, {startClean: true});
replSet.awaitReplication();
replSet.awaitSecondaryNodes();

assert.eq(1, secondary.getDB("test")[name].count());
assert.docEq({_id: 0, x: 1}, secondary.getDB("test")[name].findOne());

replSet.stopSet();
