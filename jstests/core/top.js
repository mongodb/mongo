/**
 * 1. check top numbers are correct
 *
 * This test attempts to perform read operations and get statistics using the top command. The
 * former operation may be routed to a secondary in the replica set, whereas the latter must be
 * routed to the primary.
 *
 * @tags: [
 *    # The test runs commands that are not allowed with security token: top.
 *    not_allowed_with_signed_security_token,
 *    assumes_read_preference_unchanged,
 *    requires_fastcount,
 *    # This test contains assertions on the number of executed operations, and tenant migrations
 *    # passthrough suites automatically retry operations on TenantMigrationAborted errors.
 *    tenant_migration_incompatible,
 *    does_not_support_repeated_reads,
 *    requires_fcv_62,
 * ]
 */

import {assertTopDiffEq, getTop} from "jstests/libs/stats.js";

const dbName = jsTestName();

// Skipping the collection from dbcheck during the test. See 'collNamesIgnoredFromDBCheck' in
// dbcheck_utils.js for more info.
const collName = dbName + "_coll_temp";
const afterTestCollName = dbName + "_coll";

var testDB = db.getSiblingDB(dbName);
var testColl = testDB[collName];
testColl.drop();

// Perform an operation on the collection so that it is present in the "top" command's output.
// TODO (SERVER-84307): find command against non-existent collection on sharded cluster doesn't show
// up in response to top command but the same command on replica set does.
if (TestData.testingReplicaSetEndpoint) {
    assert.commandWorked(testDB.createCollection(collName));
} else {
    assert.eq(testColl.find({}).itcount(), 0);
}

//  This variable is used to get differential output
var lastTop = getTop(testColl);
if (lastTop === undefined) {
    quit();
}

var numRecords = 100;

//  Insert
for (var i = 0; i < numRecords; i++) {
    assert.commandWorked(testColl.insert({_id: i}));
}
assertTopDiffEq(testDB, testColl, lastTop, "insert", numRecords);
lastTop = assertTopDiffEq(testDB, testColl, lastTop, "writeLock", numRecords);

// Update
for (i = 0; i < numRecords; i++) {
    assert.commandWorked(testColl.update({_id: i}, {x: i}));
}
lastTop = assertTopDiffEq(testDB, testColl, lastTop, "update", numRecords);

// Queries
var query = {};
for (i = 0; i < numRecords; i++) {
    query[i] = testColl.find({x: {$gte: i}}).batchSize(2);
    assert.eq(query[i].next()._id, i);
}
lastTop = assertTopDiffEq(testDB, testColl, lastTop, "queries", numRecords);

// Getmore
for (i = 0; i < numRecords / 2; i++) {
    assert.eq(query[i].next()._id, i + 1);
    assert.eq(query[i].next()._id, i + 2);
    assert.eq(query[i].next()._id, i + 3);
    assert.eq(query[i].next()._id, i + 4);
}
lastTop = assertTopDiffEq(testDB, testColl, lastTop, "getmore", numRecords);

// Remove
for (i = 0; i < numRecords; i++) {
    assert.commandWorked(testColl.remove({_id: 1}));
}
lastTop = assertTopDiffEq(testDB, testColl, lastTop, "remove", numRecords);

// Upsert, note that these are counted as updates, not inserts
for (i = 0; i < numRecords; i++) {
    assert.commandWorked(testColl.update({_id: i}, {x: i}, {upsert: 1}));
}
lastTop = assertTopDiffEq(testDB, testColl, lastTop, "update", numRecords);

// Commands
var res;

// "count" command
lastTop = getTop(testColl);  // ignore any commands before this
if (lastTop === undefined) {
    quit();
}

for (i = 0; i < numRecords; i++) {
    res = assert.commandWorked(testDB.runCommand({count: testColl.getName()}));
    assert.eq(res.n, numRecords, tojson(res));
}
lastTop = assertTopDiffEq(testDB, testColl, lastTop, "commands", numRecords);

// "findAndModify" command
lastTop = getTop(testColl);
if (lastTop === undefined) {
    quit();
}

for (i = 0; i < numRecords; i++) {
    res = assert.commandWorked(testDB.runCommand({
        findAndModify: testColl.getName(),
        query: {_id: i},
        update: {$inc: {x: 1}},
    }));
    assert.eq(res.value.x, i, tojson(res));
}
lastTop = assertTopDiffEq(testDB, testColl, lastTop, "commands", numRecords);

lastTop = getTop(testColl);
if (lastTop === undefined) {
    quit();
}

for (i = 0; i < numRecords; i++) {
    res = assert.commandWorked(testDB.runCommand({
        findAndModify: testColl.getName(),
        query: {_id: i},
        remove: true,
    }));
    assert.eq(res.value.x, i + 1, tojson(res));
}
lastTop = assertTopDiffEq(testDB, testColl, lastTop, "commands", numRecords);

// aggregate
assert.eq(0, testColl.aggregate([]).itcount());  // All records were just deleted.
assertTopDiffEq(testDB, testColl, lastTop, "commands", 1);
lastTop = assertTopDiffEq(testDB, testColl, lastTop, "readLock", 1);

// getIndexes
assert.eq(1, testColl.getIndexes().length);
assertTopDiffEq(testDB, testColl, lastTop, "commands", 1);
lastTop = assertTopDiffEq(testDB, testColl, lastTop, "readLock", 1);

// aggregate with $indexStats
assert.doesNotThrow(() => testColl.aggregate([{$indexStats: {}}]).itcount());
assertTopDiffEq(testDB, testColl, lastTop, "commands", 1);
lastTop = assertTopDiffEq(testDB, testColl, lastTop, "readLock", 1);

// createIndex
res = assert.commandWorked(testColl.createIndex({x: 1}));
assertTopDiffEq(testDB, testColl, lastTop, "writeLock", 1);
lastTop = assertTopDiffEq(testDB, testColl, lastTop, "commands", 1);

// dropIndex
res = assert.commandWorked(testColl.dropIndex({x: 1}));
assertTopDiffEq(testDB, testColl, lastTop, "commands", 1);
lastTop = assertTopDiffEq(testDB, testColl, lastTop, "writeLock", 1);

// Rename the collection to enable it for dbcheck after the test.
assert.commandWorked(testColl.renameCollection(afterTestCollName, true /* dropTarget */));
