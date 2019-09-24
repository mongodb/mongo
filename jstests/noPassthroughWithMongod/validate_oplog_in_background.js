/**
 * Ensures that background validation works on the Oplog.
 *
 * @tags: [requires_wiredtiger, requires_persistence]
 */
(function() {
'use strict';

let localDb = db.getSiblingDB("local");

// Create the oplog collection so background validation has something upon which to run.
const res = localDb.createCollection("oplog.rs", {capped: true, size: 4096});
if (!res.ok) {
    // burn_in_tests can run this test repeatedly on the same mongod, thus NamespaceExists must be
    // handled. The mongod server does not allow the oplog collection to be dropped to reset for the
    // next test run.
    assert.commandFailedWithCode(res, ErrorCodes.NamespaceExists);
}

// Force a WT checkpoint so that background validation finds the new collection in the checkpoint
// and can run meaningfully instead of returning early upon finding nothing to validate.
assert.commandWorked(localDb.fsyncLock());
assert.commandWorked(localDb.fsyncUnlock());

assert.commandWorked(localDb.runCommand({validate: "oplog.rs", background: true}));
}());
