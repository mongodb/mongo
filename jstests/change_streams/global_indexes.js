/**
 * Tests global index operations generate change stream events when appropriate. CRUD operations do
 * not generate events, while DDL operations do generate events.
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

// Make sure database does not exist. Otherwise burn-in may fail in passthrough.
db.getSiblingDB(jsTestName()).dropDatabase();

function runTest() {
    let cursor = cst.startWatchingAllChangesForCluster();
    const globalIndexUUID = UUID();

    assert.commandWorked(adminDB.runCommand({_shardsvrCreateGlobalIndex: globalIndexUUID}));

    // TODO (SERVER-69852): _shardsvrCreateGlobalIndex should generate a change stream event.
    cst.assertNoChange(cursor);

    // Global index insert operations should not generate change stream events.
    {
        const session = db.getMongo().startSession();
        session.startTransaction();
        assert.commandWorked(session.getDatabase("system").runCommand({
            "_shardsvrInsertGlobalIndexKey": globalIndexUUID,
            key: {a: 1},
            docKey: {sk: 1, _id: 1}
        }));
        session.commitTransaction();
        session.endSession();
    }
    cst.assertNoChange(cursor);

    // Global index delete operations should not generate change stream events.
    {
        const session = db.getMongo().startSession();
        session.startTransaction();
        assert.commandWorked(session.getDatabase("system").runCommand({
            "_shardsvrDeleteGlobalIndexKey": globalIndexUUID,
            key: {a: 1},
            docKey: {sk: 1, _id: 1}
        }));
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
        assert.commandWorked(session.getDatabase("system").runCommand({
            "_shardsvrInsertGlobalIndexKey": globalIndexUUID,
            key: {a: 1},
            docKey: {sk: 1, _id: 1}
        }));
        assert.commandWorked(session.getDatabase(jsTestName()).coll.insert({_id: 1, a: 123}));
        session.commitTransaction();
        session.endSession();
    }
    // getNextChanges will timeout if no change is found.
    const nextChange = cst.getNextChanges(cursor, 1)[0];
    assert.eq(nextChange["operationType"], "insert");
    assert.eq(nextChange["ns"], {"db": jsTestName(), "coll": "coll"});
    assert.eq(nextChange["fullDocument"], {_id: 1, a: 123});

    assert.commandWorked(adminDB.runCommand({_shardsvrDropGlobalIndex: globalIndexUUID}));

    // TODO (SERVER-69852): _shardsvrCreateDropIndex should generate a change stream event.
    cst.assertNoChange(cursor);
}

runTest();

cst.cleanUp();
}());
