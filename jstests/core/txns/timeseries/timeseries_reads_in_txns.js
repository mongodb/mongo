/**
 * Tests if queries against time-series collections in transactions work as expected.
 * @tags: [
 *     requires_fcv_49,
 *     requires_find_command,
 *     uses_transactions,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");  // For 'TimeseriesTest'.

if (!TimeseriesTest.timeseriesCollectionsEnabled(db.getMongo())) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    return;
}

const tsTestColl = db.timeseries_txn_ts_coll;
tsTestColl.drop();

const regularColl = db.timeseries_txn_regular_coll;
regularColl.drop();

assert.commandWorked(db.createCollection(regularColl.getName()));

const timeFieldName = "time";

// Create the time-series collection with data outside of a txn session because createCollection
// commands for time-series will throw ErrorCodes.OperationNotSupportedInTransaction.
assert.commandWorked(
    db.createCollection(tsTestColl.getName(), {timeseries: {timeField: timeFieldName}}));

let doc1 = {[timeFieldName]: ISODate("2021-03-01T00:00:00.000Z"), x: 1, _id: 1};
let doc2 = {[timeFieldName]: ISODate("2021-03-01T01:00:00.000Z"), x: 2, _id: 2};
let doc3 = {[timeFieldName]: ISODate("2021-03-01T02:00:00.000Z"), x: 2, _id: 3};

assert.commandWorked(tsTestColl.insert(doc1));
assert.commandWorked(tsTestColl.insert(doc2));
assert.commandWorked(tsTestColl.insert(doc3));

const session = db.getMongo().startSession();
const sessionDB = session.getDatabase(db.getName());
const sessionTsColl = sessionDB.getCollection(tsTestColl.getName());

// Test a simple find over a time-series collection.
session.startTransaction();

assert.eq(3, sessionTsColl.find().itcount());
let doc = sessionTsColl.findOne({_id: 1, x: 1});
assert.neq(null, doc);
assert.docEq(doc1, doc);

session.commitTransaction();

// Test a read from a time-series collection then a subsequent insert into a regular collection. All
// writes to time-series collections are banned within transactions.
const sessionRegularColl = sessionDB.getCollection(regularColl.getName());
session.startTransaction();

let doc4 = sessionTsColl.findOne({_id: 1, x: 1});
assert.commandWorked(sessionRegularColl.insert(doc4));

// The last insert should be visible in this session.
doc4 = sessionRegularColl.findOne({_id: 1, x: 1});
assert.neq(null, doc4);
assert.docEq(doc4, doc1);

session.commitTransaction();

// Verify that after a commit the update persists.
let doc5 = regularColl.findOne({_id: 1, x: 1});
assert.neq(null, doc5);
assert.docEq(doc5, doc4);
})();
