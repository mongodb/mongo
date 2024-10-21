/**
 * Ensures that replicating the dropDatabase oplog entry and clearing the collection catalog during
 * dropDatabase is done atomically. This prevents a problem where we step down after writing out the
 * dropDatabase oplog entry, the node becomes a secondary with the collection catalog still
 * containing the database name. If the new primary creates the same database name with a different
 * casing, the secondary would fatally assert.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 3});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();

assert.commandWorked(primary.getDB("test").createCollection("a"));

let awaitDrop = startParallelShell(() => {
    db.getSiblingDB("test").dropDatabase();
}, primary.port);
let awaitFailPoint = configureFailPoint(primary, "dropDatabaseHangBeforeInMemoryDrop");
awaitFailPoint.wait();

// Wait for secondaries to apply the dropDatabase oplog entry.
rst.awaitReplication();

// Make the primary step down before finishing dropDatabase.
assert.commandWorked(primary.getDB("admin").adminCommand({replSetStepDown: 30}));

awaitFailPoint.off();
awaitDrop();

assert.commandFailedWithCode(rst.getPrimary().getDB("TEST").createCollection("a"),
                             ErrorCodes.DatabaseDifferCase);
rst.stopSet();