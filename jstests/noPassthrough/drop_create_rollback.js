/**
 * Tests that the _mdb_catalog does not reuse RecordIds after a catalog restart.
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 *   requires_wiredtiger,
 *   sbe_incompatible,
 * ]
 */
(function() {
'use strict';

TestData.rollbackShutdowns = true;
TestData.logComponentVerbosity = {
    storage: {recovery: 2}
};
load('jstests/replsets/libs/rollback_test.js');

const rollbackTest = new RollbackTest();
let primary = rollbackTest.getPrimary();
// Do a majority write to guarantee the stable timestamp contains this create. Otherwise startup
// replication recovery will recreate the collection, initializing the _mdb_catalog RecordId
// generator.
assert.commandWorked(
    primary.getDB("foo").runCommand({create: "timestamped", writeConcern: {w: "majority"}}));

// This restart forces the _mdb_catalog to refresh, uninitializing the auto-incrementing RecordId
// generator.
jsTestLog({msg: "Restarting primary.", primary: primary, nodeId: primary.nodeId});
const SIGTERM = 15;  // clean shutdown
rollbackTest.restartNode(primary.nodeId, SIGTERM);

let rollbackNode = rollbackTest.transitionToRollbackOperations();
jsTestLog({
    msg: "The restarted primary must be the node that goes into rollback.",
    primary: primary,
    rollback: rollbackNode
});
assert.eq(primary, rollbackNode);
// The `timestamped` collection is positioned as the last record in the _mdb_catalog. An unpatched
// MongoDB dropping this collection will result in its RecordId being reused.
assert.commandWorked(rollbackNode.getDB("foo").runCommand({drop: "timestamped"}));
// Reusing the RecordId with an untimestamped write (due to being in the `local` database) results
// in an illegal update chain. A successful rollback should see the timestamped collection. But an
// unpatched MongoDB would have the untimestamped update on the same update chain, "locking in" the
// drop.
assert.commandWorked(rollbackNode.getDB("local").createCollection("untimestamped"));

rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();
rollbackTest.transitionToSteadyStateOperations();

assert.contains("timestamped", rollbackNode.getDB("foo").getCollectionNames());
assert.contains("untimestamped", rollbackNode.getDB("local").getCollectionNames());
rollbackTest.stop();
})();
