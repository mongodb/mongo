/**
 * Tests the _shardsvrInsertGlobalIndexKey and _shardsvrDeleteGlobalIndexKey commands.
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
const otherGlobalIndexUUID = UUID();

assert.commandWorked(adminDB.runCommand({_shardsvrCreateGlobalIndex: globalIndexUUID}));
assert.commandWorked(adminDB.runCommand({_shardsvrCreateGlobalIndex: otherGlobalIndexUUID}));

// _shardsvrInsertGlobalIndexKey must run inside a transaction.
assert.commandFailedWithCode(
    adminDB.runCommand(
        {_shardsvrInsertGlobalIndexKey: globalIndexUUID, key: {a: 1}, docKey: {sk: 1, _id: 1}}),
    6789400);

// _shardsvrDeleteGlobalIndexKey must run inside a transaction.
assert.commandFailedWithCode(
    adminDB.runCommand(
        {_shardsvrDeleteGlobalIndexKey: globalIndexUUID, key: {a: 1}, docKey: {sk: 1, _id: 1}}),
    6924200);

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

// Cannot delete a key from an unexisting container.
{
    const session = primary.startSession();
    session.startTransaction();
    assert.commandFailedWithCode(
        session.getDatabase("system").runCommand(
            {_shardsvrDeleteGlobalIndexKey: UUID(), key: {a: 1}, docKey: {sk: 1, _id: 1}}),
        6924201);
    session.endSession();
}

// _shardsvrInsertGlobalIndexKey and posterior _shardsvrDeleteGlobalIndexKey succeed. Run multiple
// times to ensure that operations are properly serialized on replication and do not cause
// secondaries to crash.
{
    const session = primary.startSession();
    for (var i = 0; i < 10; i++) {
        session.startTransaction();
        assert.commandWorked(session.getDatabase("system").runCommand({
            "_shardsvrInsertGlobalIndexKey": globalIndexUUID,
            key: {a: 1},
            docKey: {sk: 1, _id: i}
        }));
        assert.commandWorked(session.getDatabase("system").runCommand({
            "_shardsvrDeleteGlobalIndexKey": globalIndexUUID,
            key: {a: 1},
            docKey: {sk: 1, _id: i}
        }));
        session.commitTransaction();
    }
    session.endSession();
}

// Insert duplicate key.
{
    const session = primary.startSession();
    session.startTransaction();
    assert.commandWorked(session.getDatabase("system").runCommand(
        {"_shardsvrInsertGlobalIndexKey": globalIndexUUID, key: {a: 1}, docKey: {sk: 1, _id: 1}}));
    assert.commandFailedWithCode(session.getDatabase("system").runCommand({
        "_shardsvrInsertGlobalIndexKey": globalIndexUUID,
        key: {a: 1},
        docKey: {sk: 10, _id: 10}
    }),
                                 ErrorCodes.DuplicateKey);
    session.endSession();
}

// _shardsvrDeleteGlobalIndexKey and reinsert _shardsvrInsertGlobalIndexKey succeed. Run multiple
// times to ensure that operations are properly serialized on replication and do not cause
// secondaries to crash.
{
    const session = primary.startSession();
    session.startTransaction();
    assert.commandWorked(session.getDatabase("system").runCommand(
        {"_shardsvrInsertGlobalIndexKey": globalIndexUUID, key: {a: 1}, docKey: {sk: 1, _id: 0}}));
    session.commitTransaction();
    for (var i = 0; i < 10; i++) {
        session.startTransaction();
        assert.commandWorked(session.getDatabase("system").runCommand({
            "_shardsvrDeleteGlobalIndexKey": globalIndexUUID,
            key: {a: 1},
            docKey: {sk: 1, _id: i}
        }));
        assert.commandWorked(session.getDatabase("system").runCommand({
            "_shardsvrInsertGlobalIndexKey": globalIndexUUID,
            key: {a: 1},
            docKey: {sk: 1, _id: i + 1}
        }));
        session.commitTransaction();
    }
    session.endSession();
}

// Delete non existing key in non-empty collection.
{
    const session = primary.startSession();
    session.startTransaction();
    assert.commandFailedWithCode(session.getDatabase("system").runCommand({
        "_shardsvrDeleteGlobalIndexKey": globalIndexUUID,
        key: {a: 123},
        docKey: {sk: 1, _id: 1}
    }),
                                 ErrorCodes.KeyNotFound);
    session.endSession();
}

// Delete existing docKey with mismatched key.
{
    const session = primary.startSession();
    session.startTransaction();
    assert.commandFailedWithCode(session.getDatabase("system").runCommand({
        "_shardsvrDeleteGlobalIndexKey": globalIndexUUID,
        key: {a: 123},
        docKey: {sk: 1, _id: 10}
    }),
                                 ErrorCodes.KeyNotFound);
    session.endSession();
}

// Run _shardsvrInsertGlobalIndexKey and _shardsvrDeleteGlobalIndexKey inside a transaction that
// performs reads and writes to other namespaces. Also perform inserts and deletes to two different
// global indexes.
{
    primary.getDB("test").getCollection("c").insertOne({foo: 1});
    rst.awaitReplication();

    const session = primary.startSession();
    session.startTransaction({readConcern: {level: "snapshot"}, writeConcern: {w: "majority"}});
    assert.commandWorked(session.getDatabase("system").runCommand({
        "_shardsvrInsertGlobalIndexKey": otherGlobalIndexUUID,
        key: {a: 123},
        docKey: {sk: 123, _id: 123}
    }));
    assert.eq(1, session.getDatabase("test").getCollection("c").find({foo: 1}).itcount());
    assert.commandWorked(session.getDatabase("system").runCommand({
        "_shardsvrInsertGlobalIndexKey": globalIndexUUID,
        key: {a: 123},
        docKey: {sk: 123, _id: 123}
    }));
    assert.commandWorked(session.getDatabase("test").getCollection("c").insertOne({bar: 1}));
    assert.commandWorked(session.getDatabase("system").runCommand({
        "_shardsvrDeleteGlobalIndexKey": otherGlobalIndexUUID,
        key: {a: 123},
        docKey: {sk: 123, _id: 123}
    }));
    assert.commandWorked(session.getDatabase("system").runCommand({
        "_shardsvrDeleteGlobalIndexKey": globalIndexUUID,
        key: {a: 123},
        docKey: {sk: 123, _id: 123}
    }));
    session.commitTransaction();
    session.endSession();
}

rst.stopSet();
})();
