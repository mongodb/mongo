/**
 * Tests that the 'validate' command with 'atClusterTime' returns NamespaceNotFound for a collection
 * that did not exist at that timestamp, whether it was created afterwards or never existed.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_wiredtiger,
 *   requires_replication,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const db = primary.getDB(jsTestName());

// 'existing' exists at 'atClusterTime'.
const res = assert.commandWorked(db.runCommand({insert: "existing", documents: [{a: 1}]}));
const atClusterTime = res.operationTime;

// 'createdLate' is created after 'atClusterTime'.
assert.commandWorked(db.runCommand({insert: "createdLate", documents: [{a: 1}]}));

// A collection created after 'atClusterTime' did not exist at that time, so validation returns
// NamespaceNotFound.
assert.commandFailedWithCode(
    db.runCommand({validate: "createdLate", atClusterTime: atClusterTime}),
    ErrorCodes.NamespaceNotFound,
);

// A collection that never existed also returns NamespaceNotFound.
assert.commandFailedWithCode(
    db.runCommand({validate: "neverExisted", atClusterTime: atClusterTime}),
    ErrorCodes.NamespaceNotFound,
);

// A collection that existed at 'atClusterTime' is validated normally.
const existingRes = assert.commandWorked(
    db.runCommand({validate: "existing", atClusterTime: atClusterTime}),
);
assert(existingRes.valid, `Expected valid result: ${tojson(existingRes)}`);

rst.stopSet();
