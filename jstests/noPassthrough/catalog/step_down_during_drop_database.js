/**
 * Tests that performing a stepdown on the primary during a dropDatabase command doesn't have any
 * negative effects when the new primary runs the same dropDatabase command while the old primary
 * is still in the midst of dropping the database.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const dbName = "test";
const collName = "coll";

const replSet = new ReplSetTest({nodes: 2});
replSet.startSet();
replSet.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

let primary = replSet.getPrimary();
let testDB = primary.getDB(dbName);

const size = 5;
jsTest.log("Creating " + size + " test documents.");
let bulk = testDB.getCollection(collName).initializeUnorderedBulkOp();
for (let i = 0; i < size; ++i) {
    bulk.insert({i: i});
}
assert.commandWorked(bulk.execute());
replSet.awaitReplication();

const failpoint = "dropDatabaseHangAfterAllCollectionsDrop";
assert.commandWorked(primary.adminCommand({configureFailPoint: failpoint, mode: "alwaysOn"}));

// Run the dropDatabase command and stepdown the primary while it is running.
const awaitShell = startParallelShell(() => {
    db.dropDatabase();
}, testDB.getMongo().port);

// Ensure the dropDatabase command has begun before stepping down.
checkLog.contains(
    primary,
    "dropDatabase - fail point dropDatabaseHangAfterAllCollectionsDrop " +
        "enabled. Blocking until fail point is disabled",
);

assert.commandWorked(testDB.adminCommand({replSetStepDown: 60, force: true}));
replSet.awaitSecondaryNodes(null, [primary]);

assert.commandWorked(primary.adminCommand({configureFailPoint: failpoint, mode: "off"}));
awaitShell();

primary = replSet.getPrimary();
testDB = primary.getDB(dbName);

// Run dropDatabase on the new primary. The secondary (formerly the primary) should be able to
// drop the database too.
testDB.dropDatabase();
replSet.awaitReplication();

replSet.stopSet();
