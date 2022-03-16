/**
 * Ensures that the 'collMod' command takes a Collection MODE_X lock during a no-op.
 */
(function() {
'use strict';

load('jstests/libs/parallel_shell_helpers.js');

// Note: failpoint name may not be changed due to backwards compatibility problem in
// multiversion suites running JS tests using the failpoint.
// In reality this hangs after acquiring a collection MODE_X lock (w/ database MODE_IX)."
const failpoint = 'hangAfterDatabaseLock';
assert.commandWorked(db.adminCommand({configureFailPoint: failpoint, mode: "alwaysOn"}));

const collectionName = jsTestName();

const conn = db.getMongo();
const coll = db.getCollection(collectionName);
coll.drop();
assert.commandWorked(db.createCollection(collectionName));

// Run a no-op collMod command.
const awaitParallelShell =
    startParallelShell(funWithArgs((collectionName) => {
                           assert.commandWorked(db.runCommand({collMod: collectionName}));
                       }, collectionName), conn.port);

// Check that the Collection MODE_X lock is being held by checking in lockInfo.
assert.soon(() => {
    let lockInfo = assert.commandWorked(db.adminCommand({lockInfo: 1})).lockInfo;
    for (let i = 0; i < lockInfo.length; i++) {
        let resourceId = lockInfo[i].resourceId;
        const mode = lockInfo[i].granted[0].mode;
        if (resourceId.includes("Collection") && resourceId.includes("test." + collectionName) &&
            mode === "X") {
            return true;
        }
    }

    return false;
});

assert.commandWorked(db.adminCommand({configureFailPoint: failpoint, mode: "off"}));
awaitParallelShell();
})();
