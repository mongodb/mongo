/**
 * Tests global index maintenance API does not generate change stream events.
 *
 * @tags: [
 *     assumes_against_mongod_not_mongos,
 *     featureFlagGlobalIndexes,
 *     requires_fcv_62,
 *     uses_transactions
 * ]
 */

// TODO (SERVER-69932): once sharding has a working global index implementation, rewrite this test
// to run on mongos (don't use internal commands) and remove assumes_against_mongod_not_mongos.

(function() {
"use strict";

load("jstests/libs/change_stream_util.js");

const adminDB = db.getSiblingDB("admin");
const cst = new ChangeStreamTest(adminDB);

// Drop database and recreate collection to avoid changestream event for collection create and
// make sure collection is empty.
db.getSiblingDB(jsTestName()).dropDatabase();
db.getSiblingDB(jsTestName()).createCollection("coll");

let cursor = cst.startWatchingChanges({
    pipeline: [{
        $changeStream:
            {allChangesForCluster: true, showExpandedEvents: true, showSystemEvents: true}
    }],
    collection: 1
});
const globalIndexUUID = UUID();

// _shardsvrCreateGlobalIndex should not generate a change stream.
assert.commandWorked(adminDB.runCommand({_shardsvrCreateGlobalIndex: globalIndexUUID}));
cst.assertNoChange(cursor);

// Global index insert operations should not generate change stream events.
{
    const session = db.getMongo().startSession();
    session.startTransaction();
    assert.commandWorked(session.getDatabase("system").runCommand(
        {"_shardsvrInsertGlobalIndexKey": globalIndexUUID, key: {a: 1}, docKey: {sk: 1, _id: 1}}));
    session.commitTransaction();
    session.endSession();
}
cst.assertNoChange(cursor);

// Global index delete operations should not generate change stream events.
{
    const session = db.getMongo().startSession();
    session.startTransaction();
    assert.commandWorked(session.getDatabase("system").runCommand(
        {"_shardsvrDeleteGlobalIndexKey": globalIndexUUID, key: {a: 1}, docKey: {sk: 1, _id: 1}}));
    session.commitTransaction();
    session.endSession();
}
cst.assertNoChange(cursor);

// Global index bulk operations should not generate change stream events.
{
    const session = db.getMongo().startSession();
    session.startTransaction();
    assert.commandWorked(session.getDatabase("system").runCommand({
        _shardsvrWriteGlobalIndexKeys: 1,
        ops: [
            {
                _shardsvrInsertGlobalIndexKey: globalIndexUUID,
                key: {myKey: "abc"},
                docKey: {sk: 123, _id: 987}
            },
            {
                _shardsvrDeleteGlobalIndexKey: globalIndexUUID,
                key: {myKey: "abc"},
                docKey: {sk: 123, _id: 987}
            }
        ]
    }));
    session.commitTransaction();
    session.endSession();
}
cst.assertNoChange(cursor);

// Validate behaviour of change streams events for regular operations is preserved when used
// together with global indexes.
{
    const session = db.getMongo().startSession();
    session.startTransaction();
    assert.commandWorked(session.getDatabase("system").runCommand(
        {"_shardsvrInsertGlobalIndexKey": globalIndexUUID, key: {a: 1}, docKey: {sk: 1, _id: 1}}));
    assert.commandWorked(session.getDatabase(jsTestName()).coll.insert({_id: 1, a: 123}));
    session.commitTransaction();
    session.endSession();
}
// getNextChanges will timeout if no change is found.
const nextChange = cst.getNextChanges(cursor, 1)[0];
const expectedInsert = {
    operationType: "insert",
    fullDocument: {_id: 1, a: 123},
    ns: {db: jsTestName(), coll: "coll"},
    documentKey: {_id: 1}
};
assertChangeStreamEventEq(nextChange, expectedInsert);

// _shardsvrDropGlobalIndex should not generate a change stream.
assert.commandWorked(adminDB.runCommand({_shardsvrDropGlobalIndex: globalIndexUUID}));
cst.assertNoChange(cursor);

cst.cleanUp();
}());
