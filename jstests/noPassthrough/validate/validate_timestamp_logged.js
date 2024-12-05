/**
 * Tests the validate command reports the readTimestamp only when used.
 *
 * Checks that {background:true} on a primary yields the readTimestamp
 * Checks that {background:false} on a primary doesn't yield the readTimestamp
 * Checks that {background:true} on a secondary yields the readTimestamp
 * Checks that {background:false} on a secondary yields the readTimestamp
 *
 * @tags: [requires_fsync, requires_wiredtiger, requires_persistence]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const secondary = rst.getSecondary();

const dbName = "test_db_background_validation";
const collName = "test_coll_background_validation";

const primaryColl = primary.getDB(dbName).getCollection(collName);
const secondaryColl = secondary.getDB(dbName).getCollection(collName);

primaryColl.drop();

// Create some indexes and insert some data, so we can validate them more meaningfully.
assert.commandWorked(primaryColl.createIndex({a: 1}));
assert.commandWorked(primaryColl.createIndex({b: 1}));
assert.commandWorked(primaryColl.createIndex({c: 1}));

const numDocs = 100;
for (let i = 0; i < numDocs; ++i) {
    assert.commandWorked(primaryColl.insert({a: i, b: i, c: i}));
}

rst.awaitReplication();
rst.awaitLastStableRecoveryTimestamp();
assert.commandWorked(primary.getDB(dbName).adminCommand({fsync: 1}));
assert.commandWorked(secondary.getDB(dbName).adminCommand({fsync: 1}));

// Check that validate on primary in background has timestamp
let primaryBackgroundRes = primaryColl.validate({background: true});
jsTestLog(primaryBackgroundRes);
assert.commandWorked(primaryBackgroundRes);
assert(primaryBackgroundRes.hasOwnProperty('readTimestamp'));

// Check that validate on primary in foreground does NOT have timestamp.
let primaryForegroundRes = primaryColl.validate({background: false});
jsTestLog(primaryForegroundRes);
assert.commandWorked(primaryForegroundRes);
assert(!primaryForegroundRes.hasOwnProperty('readTimestamp'));

// Check that validate on secondary in background has timestamp
let secondaryBackgroundRes = secondaryColl.validate({background: true});
jsTestLog(secondaryBackgroundRes);
assert.commandWorked(secondaryBackgroundRes);
assert(secondaryBackgroundRes.hasOwnProperty('readTimestamp'));

// Check that validate on secondary in foreground does have timestamp
let secondaryForegroundRes = secondaryColl.validate({background: false});
jsTestLog(secondaryForegroundRes);
assert.commandWorked(secondaryForegroundRes);
assert(secondaryForegroundRes.hasOwnProperty('readTimestamp'));

rst.stopSet();
