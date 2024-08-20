/**
 * Verifies that collection drops that aren't replicated do not drop the ident until the catalog has
 * been checkpointed.
 * @tags: [
 *    requires_persistence,
 *    requires_replication,
 *    requires_wiredtiger,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Disable the checkpoint thread since we want to perform the checkpoint at a later stage.
const rst = new ReplSetTest({
    name: "correct",
    nodes: 2,
    nodeOptions: {setParameter: {syncdelay: 0, logComponentVerbosity: '{verbosity: 1}'}}
});
rst.startSet();
rst.initiate();

const prim = rst.getPrimary();
// Create a collection on the local database so we perform untimestamped writes on it.
const coll = prim.getDB("local")[jsTestName()];
coll.insertOne({a: 1});

let currentCount = checkLog.getFilteredLogMessages(prim, 8097401, {}).length;
const currentDroppedCollections = checkLog.getFilteredLogMessages(prim, 6776600, {}).length;

// Drop the local collection, the ident should not be dropped as we haven't checkpointed yet.
coll.drop();

assert.soon(() => checkLog.checkContainsWithAtLeastCountJson(prim, 8097401, {}, currentCount + 1));
// Verify that the ident hasn't been dropped since the catalog hasn't been checkpointed.
assert.eq(
    checkLog.checkContainsWithAtLeastCountJson(prim, 6776600, {}, currentDroppedCollections + 1),
    false);

// We force a checkpoint, this will make the catalog get checkpointed and let the ident drops
// through.
prim.adminCommand({fsync: 1});

// Wait until the ident has been removed.
assert.soon(() => checkLog.checkContainsWithAtLeastCountJson(
                prim, 6776600, {}, currentDroppedCollections + 1));

rst.stopSet();
