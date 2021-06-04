/**
 * Capped collections perform un-timestamped writes to delete expired documents in FCV 4.4. As a
 * result, background validation will fail when reading documents that have just been deleted.
 *
 * @tags: [
 *     # ephemeralForTest does not support background validation.
 *     incompatible_with_eft,
 *     requires_capped,
 *     requires_replication,
 * ]
 */
(function() {
'use strict';

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const dbName = "test";
const nonCappedCollName = "non-capped";
const cappedCollName = "capped";

const testDB = rst.getPrimary().getDB(dbName);
assert.commandWorked(testDB.createCollection(nonCappedCollName));
assert.commandWorked(testDB.createCollection(cappedCollName, {capped: true, size: 4096}));

const nonCappedColl = testDB.getCollection(nonCappedCollName);
const cappedColl = testDB.getCollection(cappedCollName);

// Background validation is supported in FCV 5.0 on non-capped collections.
assert.commandWorked(nonCappedColl.validate({background: true}));
assert.commandWorked(nonCappedColl.validate({background: false}));

// Background validation is supported in FCV 5.0 on capped collections.
assert.commandWorked(cappedColl.validate({background: true}));
assert.commandWorked(cappedColl.validate({background: false}));

assert.commandWorked(rst.getPrimary().adminCommand({setFeatureCompatibilityVersion: "4.4"}));

// Background validation is supported in FCV 4.4 on non-capped collections.
assert.commandWorked(nonCappedColl.validate({background: true}));
assert.commandWorked(nonCappedColl.validate({background: false}));

// Background validation is not supported in FCV 4.4 on capped collections.
assert.commandFailedWithCode(cappedColl.validate({background: true}),
                             ErrorCodes.CommandNotSupported);
assert.commandWorked(cappedColl.validate({background: false}));

rst.stopSet();
}());
