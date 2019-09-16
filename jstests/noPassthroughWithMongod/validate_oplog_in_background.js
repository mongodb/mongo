/**
 * Ensures that background validation works on the Oplog.
 *
 * @tags: [requires_wiredtiger, requires_persistence]
 */
(function() {
'use strict';

let localDb = db.getSiblingDB("local");
assert.commandWorked(localDb.createCollection("oplog.rs", {capped: true, size: 4096}));

assert.commandWorked(localDb.runCommand({validate: "oplog.rs", background: true}));
}());
