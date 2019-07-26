/**
 * Ensures that the 'collMod' command takes a database MODE_X lock during a no-op.
 */
(function() {
'use strict';

const failpoint = 'hangAfterDatabaseLock';
assert.commandWorked(db.adminCommand({configureFailPoint: failpoint, mode: "alwaysOn"}));

const conn = db.getMongo();
db.createCollection('foo');

// Run a no-op collMod command.
const awaitParallelShell = startParallelShell(() => {
    assert.commandWorked(db.runCommand({collMod: 'foo'}));
}, conn.port);

// Check that the database MODE_X lock is being held by checking in lockInfo.
assert.soon(() => {
    let lockInfo = assert.commandWorked(db.adminCommand({lockInfo: 1})).lockInfo;
    for (let i = 0; i < lockInfo.length; i++) {
        let resourceId = lockInfo[i].resourceId;
        if (resourceId.includes("Database") && resourceId.includes("test")) {
            return true;
        }
    }

    return false;
});

assert.commandWorked(db.adminCommand({configureFailPoint: failpoint, mode: "off"}));
awaitParallelShell();
})();
