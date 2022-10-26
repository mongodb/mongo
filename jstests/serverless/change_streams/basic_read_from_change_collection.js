// Tests that a change stream can be opened on a change collection when one exists, and that an
// exception is thrown if we attempt to open a stream while change streams are disabled.
// @tags: [
//   requires_fcv_62,
//   assumes_against_mongod_not_mongos,
// ]

(function() {
"use strict";

// For ChangeStreamMultitenantReplicaSetTest.
load("jstests/serverless/libs/change_collection_util.js");
// For assertDropAndRecreateCollection.
load("jstests/libs/collection_drop_recreate.js");

const replSetTest = new ChangeStreamMultitenantReplicaSetTest({nodes: 2});
const primary = replSetTest.getPrimary();

// Hard code tenants id such that the tenant can be identified deterministically.
const tenantId = ObjectId("6303b6bb84305d2266d0b779");

// Connection to the replica set primary that is stamped with the tenant id.
const tenantConn =
    ChangeStreamMultitenantReplicaSetTest.getTenantConnection(primary.host, tenantId);

// Verify that the change stream observes expected events.
function verifyChangeEvents(csCursor, expectedEvents) {
    for (const [expectedOpType, expectedDoc] of expectedEvents) {
        assert.soon(() => csCursor.hasNext());
        const event = csCursor.next();

        assert.eq(event.operationType, expectedOpType, event);
        if (event.operationType == "insert") {
            assert.eq(event.fullDocument, expectedDoc);
        } else if (event.operationType == "drop") {
            assert.soon(() => csCursor.hasNext());
            assert.eq(csCursor.isClosed(), true);
        }
    }
}

// Enable change stream for the first tenant.
replSetTest.setChangeStreamState(tenantConn, true);

// Open the change stream cursor.
const testDb = tenantConn.getDB("test");
const csCursor = testDb.stockPrice.watch([]);

// Insert documents to the 'stockPrice' collection.
const docs = [{_id: "mdb", price: 250}, {_id: "tsla", price: 650}];
docs.forEach(doc => assert.commandWorked(testDb.stockPrice.insert(doc)));

// Drop the stock price collection to invalidate the change stream cursor.
assert(testDb.stockPrice.drop());

// Verify that the change stream observes the required event.
verifyChangeEvents(csCursor, [["insert", docs[0]], ["insert", docs[1]], ["drop", []]]);

// Disable and then enable the change stream.
replSetTest.setChangeStreamState(tenantConn, false);
replSetTest.setChangeStreamState(tenantConn, true);

// Add a new document to the 'stockPrice' collection and verify that re-enabling the change
// stream works correctly.
const newCsCursor = testDb.stockPrice.watch([]);
const newDocs = [{_id: "goog", price: 2000}];
newDocs.forEach(doc => assert.commandWorked(testDb.stockPrice.insert(doc)));
verifyChangeEvents(newCsCursor, [["insert", newDocs[0]]]);

// Disable the change stream while the change stream cursor is still opened.
replSetTest.setChangeStreamState(tenantConn, false);

// Verify that the cursor throws 'QueryPlanKilled' exception on doing get next.
assert.throwsWithCode(() => assert.soon(() => newCsCursor.hasNext()), ErrorCodes.QueryPlanKilled);

// Open a new change stream cursor with change stream disabled state and verify that
// 'ChangeStreamNotEnabled' exception is thrown.
assert.throwsWithCode(() => testDb.stock.watch([]), ErrorCodes.ChangeStreamNotEnabled);

replSetTest.stopSet();
}());
