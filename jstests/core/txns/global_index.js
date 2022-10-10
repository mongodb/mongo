/**
 * Tests creating, dropping, and writing to a global index container on a shard.
 *
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   # Do not implicitly create columnstore indexes.
 *   assumes_no_implicit_index_creation,
 *   featureFlagGlobalIndexes,
 *   requires_fcv_62,
 *   requires_persistence,
 *   requires_replication,
 *   requires_wiredtiger,
 *   uses_transactions,
 * ]
 */

(function() {
load('jstests/libs/uuid_util.js');

function container(uuid) {
    return db.getSiblingDB("system").getCollection("globalIndex." + extractUUIDFromObject(uuid));
}

const uuid0 = UUID();
const uuid1 = UUID();

jsTestLog("Creating the two global index containers with uuid0=" + uuid0 + ", uuid1=" + uuid1 +
          ".");
assert.commandWorked(db.getSiblingDB("admin").runCommand({_shardsvrCreateGlobalIndex: uuid0}));
assert.commandWorked(db.getSiblingDB("admin").runCommand({_shardsvrCreateGlobalIndex: uuid1}));

jsTestLog("Inserting key {k0: 0, k1: 'z'} to uuid0=" + uuid0 + ".");
{
    const session = db.getMongo().startSession();
    session.startTransaction();
    assert.commandWorked(session.getDatabase("test").runCommand({
        "_shardsvrInsertGlobalIndexKey": uuid0,
        key: {k0: 0, k1: "z"},
        docKey: {sk: "sk0", _id: 0}
    }));
    session.commitTransaction();
    session.endSession();
    assert.eq(1, container(uuid0).find().itcount());
    assert.eq(
        1,
        container(uuid0)
            .find(
                {_id: {sk: "sk0", _id: 0}, "ik": BinData(0, "KTx6AAQ="), "tb": BinData(0, "AQ==")})
            .itcount());
}

jsTestLog("Updating key {k0: 0, k1: 'z'} -> {k0: 1, k1: 'o'} to uuid0=" + uuid0 + ".");
{
    const session = db.getMongo().startSession();
    session.startTransaction();
    assert.commandWorked(session.getDatabase("dontcare").runCommand({
        "_shardsvrWriteGlobalIndexKeys": 1,
        ops: [
            {
                "_shardsvrDeleteGlobalIndexKey": uuid0,
                key: {k0: 0, k1: "z"},
                docKey: {sk: "sk0", _id: 0}
            },
            {
                "_shardsvrInsertGlobalIndexKey": uuid0,
                key: {k0: 1, k1: "o"},
                docKey: {sk: "sk0", _id: 0}
            },
        ]
    }));
    session.commitTransaction();
    session.endSession();

    assert.eq(1, container(uuid0).find().itcount());
    assert.eq(
        1,
        container(uuid0)
            .find(
                {_id: {sk: "sk0", _id: 0}, "ik": BinData(0, "KwI8bwAE"), "tb": BinData(0, "AQ==")})
            .itcount());
}

jsTestLog("Updating key {k0: 0, k1: 'o'} or another key with the same docKey {sk: 'sk0', _id: 0} " +
          "to uuid0=" + uuid0 + " returns DuplicateKey error.");
{
    const session = db.getMongo().startSession();
    session.startTransaction();
    assert.commandFailedWithCode(session.getDatabase("other").runCommand({
        "_shardsvrInsertGlobalIndexKey": uuid0,
        key: {k0: 1, k1: "o"},
        docKey: {sk: "aaa", _id: 0}
    }),
                                 ErrorCodes.DuplicateKey);
    session.abortTransaction();
    session.startTransaction();
    assert.commandFailedWithCode(session.getDatabase("other").runCommand({
        "_shardsvrInsertGlobalIndexKey": uuid0,
        key: {k0: 123, k1: "one"},
        docKey: {sk: "sk0", _id: 0}
    }),
                                 ErrorCodes.DuplicateKey);
    session.abortTransaction();
    session.endSession();
    assert.eq(1, container(uuid0).find().itcount());
}

jsTestLog("Deleting key {k0: 1, k1: 'o'} from uuid0=" + uuid0 + ".");
{
    const session = db.getMongo().startSession();
    session.startTransaction();
    assert.commandWorked(session.getDatabase("test").runCommand({
        "_shardsvrDeleteGlobalIndexKey": uuid0,
        key: {k0: 1, k1: "o"},
        docKey: {sk: "sk0", _id: 0}
    }));
    session.commitTransaction();
    session.endSession();

    assert.eq(0, container(uuid0).find().itcount());
}

jsTestLog("Inserting and removing 200 keys in bulk on uuid0=" + uuid0 + " and uuid1=" + uuid1 +
          ".");
{
    let ops = [];
    for (let i = 0; i < 100; i++) {
        ops.push({_shardsvrInsertGlobalIndexKey: uuid0, key: {a: i}, docKey: {sk: i, _id: i}});
        ops.push({_shardsvrDeleteGlobalIndexKey: uuid0, key: {a: i}, docKey: {sk: i, _id: i}});
        ops.push(
            {_shardsvrInsertGlobalIndexKey: uuid1, key: {a: "abc" + i}, docKey: {sk: i, _id: i}});
        ops.push(
            {_shardsvrDeleteGlobalIndexKey: uuid1, key: {a: "abc" + i}, docKey: {sk: i, _id: i}});
    }

    const session = db.getMongo().startSession();
    session.startTransaction();
    assert.commandWorked(
        session.getDatabase("dontcare").runCommand({"_shardsvrWriteGlobalIndexKeys": 1, ops: ops}));
    session.commitTransaction();
    session.endSession();

    assert.eq(0, container(uuid0).find().itcount());
    assert.eq(0, container(uuid1).find().itcount());
}

jsTestLog("Inserting 200 keys again in bulk on uuid0=" + uuid0 + " and uuid1=" + uuid1 + ". Then " +
          "updating those keys in a second bulk. Then re-inserting the original 200 keys, with " +
          "different docKey's.");
{
    let opsInsert = [];
    for (let i = 0; i < 100; i++) {
        opsInsert.push(
            {_shardsvrInsertGlobalIndexKey: uuid0, key: {a: i}, docKey: {sk: i, _id: i}});
        opsInsert.push(
            {_shardsvrInsertGlobalIndexKey: uuid1, key: {a: "abc" + i}, docKey: {sk: i, _id: i}});
    }

    const session = db.getMongo().startSession();
    session.startTransaction();
    assert.commandWorked(session.getDatabase("dontcare")
                             .runCommand({"_shardsvrWriteGlobalIndexKeys": 1, ops: opsInsert}));
    session.commitTransaction();

    assert.eq(100, container(uuid0).find().itcount());
    assert.eq(100, container(uuid1).find().itcount());

    session.startTransaction();
    assert.commandFailedWithCode(
        session.getDatabase("dontcare")
            .runCommand({"_shardsvrWriteGlobalIndexKeys": 1, ops: opsInsert}),
        ErrorCodes.DuplicateKey);
    session.abortTransaction();

    let opsUpdate = [];
    for (let i = 0; i < 100; i++) {
        opsUpdate.push(
            {_shardsvrDeleteGlobalIndexKey: uuid0, key: {a: i}, docKey: {sk: i, _id: i}});
        opsUpdate.push(
            {_shardsvrInsertGlobalIndexKey: uuid0, key: {a: i + 1000}, docKey: {sk: i, _id: i}});
        opsUpdate.push(
            {_shardsvrDeleteGlobalIndexKey: uuid1, key: {a: "abc" + i}, docKey: {sk: i, _id: i}});
        opsUpdate.push({
            _shardsvrInsertGlobalIndexKey: uuid1,
            key: {a: "abc" + i + 2000},
            docKey: {sk: i, _id: i}
        });
    }

    session.startTransaction();
    assert.commandWorked(session.getDatabase("dontcare")
                             .runCommand({"_shardsvrWriteGlobalIndexKeys": 1, ops: opsUpdate}));
    session.commitTransaction();

    assert.eq(100, container(uuid0).find().itcount());
    assert.eq(100, container(uuid1).find().itcount());

    for (let op of opsInsert) {
        if (op["_shardsvrInsertGlobalIndexKey"] === uuid1 ||
            op["_shardsvrDeleteGlobalIndexKey"] === uuid1) {
            op["docKey"]["sk"] += 2000;
        } else {
            op["docKey"]["sk"] += 1000;
        }
    }

    session.startTransaction();
    assert.commandWorked(session.getDatabase("dontcare")
                             .runCommand({"_shardsvrWriteGlobalIndexKeys": 1, ops: opsInsert}));
    session.commitTransaction();

    assert.eq(200, container(uuid0).find().itcount());
    assert.eq(200, container(uuid1).find().itcount());

    for (let i = 0; i < 100; i++) {
        assert.eq(1, container(uuid0).find({"_id": {sk: i + 1000, _id: i}}).itcount());
        assert.eq(1, container(uuid0).find({"_id": {sk: i, _id: i}}).itcount());
        assert.eq(1, container(uuid1).find({"_id": {sk: i + 2000, _id: i}}).itcount());
        assert.eq(1, container(uuid1).find({"_id": {sk: i, _id: i}}).itcount());
    }
    session.endSession();
}

jsTestLog("A duplicate key error inserting to uuid0=" + uuid0 + " rolls back the whole " +
          "transaction, including writes to uuid1=" + uuid1 + ".");
{
    assert.eq(200, container(uuid0).find().itcount());
    assert.eq(200, container(uuid1).find().itcount());

    const session = db.getMongo().startSession();
    session.startTransaction();
    assert.commandFailedWithCode(session.getDatabase("dontcare").runCommand({
        "_shardsvrWriteGlobalIndexKeys": 1,
        ops: [
            {
                "_shardsvrInsertGlobalIndexKey": uuid0,
                key: {k0: "new", k1: "key"},
                docKey: {sk: "new", _id: "key"}
            },
            {
                "_shardsvrInsertGlobalIndexKey": uuid1,
                key: {k0: "new", k1: "key"},
                docKey: {sk: "new", _id: "key"}
            },
            {
                "_shardsvrDeleteGlobalIndexKey": uuid1,
                key: {a: "abc" + 0},
                docKey: {sk: 2000, _id: 0}
            },
            // Trigger duplicateKey exception on 'key'.
            {
                "_shardsvrInsertGlobalIndexKey": uuid1,
                key: {a: "abc" + 1},
                docKey: {sk: 123456, _id: 0}
            },

        ]
    }),
                                 ErrorCodes.DuplicateKey);
    session.abortTransaction();
    session.endSession();

    assert.eq(200, container(uuid0).find().itcount());
    assert.eq(200, container(uuid1).find().itcount());
    for (let i = 0; i < 100; i++) {
        assert.eq(1, container(uuid0).find({"_id": {sk: i + 1000, _id: i}}).itcount());
        assert.eq(1, container(uuid0).find({"_id": {sk: i, _id: i}}).itcount());
        assert.eq(1, container(uuid1).find({"_id": {sk: i + 2000, _id: i}}).itcount());
        assert.eq(1, container(uuid1).find({"_id": {sk: i, _id: i}}).itcount());
    }
}

jsTestLog("Dropping global index uuid0=" + uuid0 + " and validating that transactions including" +
          "writes to that global indexes abort and roll back in full.");
{
    assert.commandWorked(db.getSiblingDB("admin").runCommand({_shardsvrDropGlobalIndex: uuid0}));

    const session = db.getMongo().startSession();

    session.startTransaction();
    assert.commandFailedWithCode(session.getDatabase("dontcare").runCommand({
        "_shardsvrInsertGlobalIndexKey": uuid0,
        key: {k0: 0, k1: "z"},
        docKey: {sk: "sk0", _id: 0}
    }),
                                 6789402);
    session.abortTransaction();

    assert.eq(200, container(uuid1).find().itcount());
    session.startTransaction();
    assert.commandFailedWithCode(session.getDatabase("dontcare").runCommand({
        "_shardsvrWriteGlobalIndexKeys": 1,
        ops: [
            {
                "_shardsvrInsertGlobalIndexKey": uuid1,
                key: {k0: "new", k1: "key"},
                docKey: {sk: "new", _id: "docKey"}
            },
            {
                "_shardsvrDeleteGlobalIndexKey": uuid0,
                key: {k0: "new", k1: "key"},
                docKey: {sk: "new", _id: "docKey"}
            },
        ]
    }),
                                 6924201);
    session.abortTransaction();
    assert.eq(200, container(uuid1).find().itcount());

    session.endSession();
}

jsTestLog("Dropping global index uuid1=" + uuid1 + ".");
assert.commandWorked(db.getSiblingDB("admin").runCommand({_shardsvrDropGlobalIndex: uuid1}));
})();
