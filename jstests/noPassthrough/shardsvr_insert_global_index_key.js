/**
 * Tests the _shardsvrInsertGlobalIndexKey command.
 *
 * @tags: [
 *     featureFlagGlobalIndexes,
 *     requires_fcv_62,
 *     requires_replication,
 * ]
 */
(function() {
"use strict";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();
const primary = rst.getPrimary();
const adminDB = primary.getDB("admin");
const globalIndexUUID = UUID();

assert.commandWorked(adminDB.runCommand({_shardsvrCreateGlobalIndex: globalIndexUUID}));

// _shardsvrInsertGlobalIndexKey must run inside a transaction.
assert.commandFailedWithCode(
    adminDB.runCommand(
        {_shardsvrInsertGlobalIndexKey: globalIndexUUID, key: {a: 1}, docKey: {sk: 1, _id: 1}}),
    6789400);

// Cannot insert a key into an unexisting container.
{
    const session = primary.startSession();
    session.startTransaction();
    assert.commandFailedWithCode(
        session.getDatabase("system").runCommand(
            {_shardsvrInsertGlobalIndexKey: UUID(), key: {a: 1}, docKey: {sk: 1, _id: 1}}),
        6789402);
    session.endSession();
}

// _shardsvrInsertGlobalIndexKey succeeds.
{
    const session = primary.startSession();
    session.startTransaction();
    assert.commandWorked(session.getDatabase("system").runCommand(
        {"_shardsvrInsertGlobalIndexKey": globalIndexUUID, key: {a: 1}, docKey: {sk: 1, _id: 1}}));
    session.commitTransaction();
    session.endSession();
}

// Duplicate key.
{
    const session = primary.startSession();
    session.startTransaction();
    assert.commandFailedWithCode(session.getDatabase("system").runCommand({
        "_shardsvrInsertGlobalIndexKey": globalIndexUUID,
        key: {a: 1},
        docKey: {sk: 10, _id: 10}
    }),
                                 ErrorCodes.DuplicateKey);
    session.endSession();
}

// Run _shardsvrInsertGlobalIndexKey inside a transaction that performs reads and writes to
// other namespaces.
{
    primary.getDB("test").getCollection("c").insertOne({foo: 1});
    rst.awaitReplication();

    const session = primary.startSession();
    session.startTransaction({readConcern: {level: "snapshot"}, writeConcern: {w: "majority"}});
    assert.eq(1, session.getDatabase("test").getCollection("c").find({foo: 1}).itcount());
    assert.commandWorked(session.getDatabase("system").runCommand({
        "_shardsvrInsertGlobalIndexKey": globalIndexUUID,
        key: {a: 123},
        docKey: {sk: 123, _id: 123}
    }));
    assert.commandWorked(session.getDatabase("test").getCollection("c").insertOne({bar: 1}));
    session.commitTransaction();
    session.endSession();
}

rst.stopSet();
})();
