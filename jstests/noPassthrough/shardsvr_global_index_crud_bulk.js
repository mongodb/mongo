/**
 * Tests _shardsvrWriteGlobalIndexKeys, which is sometimes referred to as 'bulk write' as it runs
 * multiple _shardsvrInsertGlobalIndexKey and _shardsvrDeleteGlobalIndexKey statements in bulk.
 *
 * @tags: [
 *     featureFlagGlobalIndexes,
 *     requires_fcv_62,
 *     requires_replication,
 * ]
 */

(function() {
"use strict";

load('jstests/libs/uuid_util.js');

function entriesInContainer(primary, uuid) {
    return primary.getDB("system")
        .getCollection("globalIndex." + extractUUIDFromObject(uuid))
        .find()
        .itcount();
}

// Speed up test time.
const maxNumberOfTxnOpsInSingleOplogEntry = 1000;

const rst = new ReplSetTest({
    nodes: 2,
    nodeOptions: {
        setParameter: {
            maxNumberOfTransactionOperationsInSingleOplogEntry: maxNumberOfTxnOpsInSingleOplogEntry
        }
    }
});
rst.startSet();
rst.initiate();
const primary = rst.getPrimary();
const adminDB = primary.getDB("admin");
const globalIndexUUID = UUID();
const session = primary.startSession();

assert.commandWorked(adminDB.runCommand({_shardsvrCreateGlobalIndex: globalIndexUUID}));
assert.eq(0, entriesInContainer(primary, globalIndexUUID));

// _shardsvrWriteGlobalIndexKeys must run inside a transaction.
assert.commandFailedWithCode(adminDB.runCommand({_shardsvrWriteGlobalIndexKeys: 1, ops: []}),
                             6789500);

// Missing required '_shardsvrWriteGlobalIndexKeys.ops' field.
{
    session.startTransaction();
    assert.commandFailedWithCode(
        session.getDatabase("test").runCommand({_shardsvrWriteGlobalIndexKeys: 1}), 40414);
    session.abortTransaction();
}

// _shardsvrWriteGlobalIndexKeys requires at least one statement in the 'ops' array.
{
    const containerEntriesBefore = entriesInContainer(primary, globalIndexUUID);

    session.startTransaction();
    assert.commandFailedWithCode(
        session.getDatabase("test").runCommand({_shardsvrWriteGlobalIndexKeys: 1, ops: []}),
        6789502);
    session.abortTransaction();
    const containerEntriesAfter = entriesInContainer(primary, globalIndexUUID);

    assert.eq(containerEntriesAfter, containerEntriesBefore);
}

// Only _shardsvrInsertGlobalIndexKey and _shardsvrDeleteGlobalIndexKey commands are allowed.
{
    session.startTransaction();
    assert.commandFailedWithCode(session.getDatabase("test").runCommand(
                                     {_shardsvrWriteGlobalIndexKeys: 1, ops: [{otherCommand: 1}]}),
                                 6789501);
    session.abortTransaction();
}

// Invalid UUID in _shardsvrWriteGlobalIndexKeys statement.
{
    session.startTransaction();
    assert.commandFailedWithCode(session.getDatabase("test").runCommand({
        _shardsvrWriteGlobalIndexKeys: 1,
        ops: [{
            "_shardsvrInsertGlobalIndexKey": "not_a_uuid",
            key: {a: "hola"},
            docKey: {sk: 3, _id: 3}
        }]
    }),
                                 ErrorCodes.InvalidUUID);
    session.abortTransaction();
}

// Unexisting UUID in statement.
{
    session.startTransaction();
    assert.commandFailedWithCode(session.getDatabase("test").runCommand({
        _shardsvrWriteGlobalIndexKeys: 1,
        ops: [{"_shardsvrInsertGlobalIndexKey": UUID(), key: {a: "hola"}, docKey: {sk: 3, _id: 3}}]
    }),
                                 6789402);
    session.abortTransaction();
}

// Missing 'key' field in statement.
{
    session.startTransaction();
    assert.commandFailedWithCode(session.getDatabase("test").runCommand({
        _shardsvrWriteGlobalIndexKeys: 1,
        ops: [{"_shardsvrInsertGlobalIndexKey": globalIndexUUID, docKey: {sk: 3, _id: 3}}]
    }),
                                 40414);
    session.abortTransaction();
}

// Missing 'docKey' field in statement.
{
    session.startTransaction();
    assert.commandFailedWithCode(session.getDatabase("test").runCommand({
        _shardsvrWriteGlobalIndexKeys: 1,
        ops: [{"_shardsvrInsertGlobalIndexKey": globalIndexUUID, key: {a: "hola"}}]
    }),
                                 40414);
    session.abortTransaction();
}

// Bulk insert a single entry, then bulk delete it.
{
    const containerEntriesBefore = entriesInContainer(primary, globalIndexUUID);

    session.startTransaction();
    assert.commandWorked(session.getDatabase("test").runCommand({
        _shardsvrWriteGlobalIndexKeys: 1,
        ops: [{
            _shardsvrInsertGlobalIndexKey: globalIndexUUID,
            key: {myKey: "abc"},
            docKey: {s: 123, _id: 987}
        }]
    }));
    session.commitTransaction();

    const containerEntriesAfterInsert = entriesInContainer(primary, globalIndexUUID);
    assert.eq(containerEntriesAfterInsert, containerEntriesBefore + 1);

    session.startTransaction();
    assert.commandWorked(session.getDatabase("test").runCommand({
        _shardsvrWriteGlobalIndexKeys: 1,
        ops: [{
            _shardsvrDeleteGlobalIndexKey: globalIndexUUID,
            key: {myKey: "abc"},
            docKey: {s: 123, _id: 987}
        }]
    }));
    session.commitTransaction();

    const containerEntriesAfterDelete = entriesInContainer(primary, globalIndexUUID);
    assert.eq(containerEntriesAfterDelete, containerEntriesBefore);
}

// Update an index key.
{
    const containerEntriesBefore = entriesInContainer(primary, globalIndexUUID);

    session.startTransaction();
    assert.commandWorked(session.getDatabase("test").runCommand({
        _shardsvrWriteGlobalIndexKeys: 1,
        ops: [{
            _shardsvrInsertGlobalIndexKey: globalIndexUUID,
            key: {myKey: "the quick brown fox"},
            docKey: {myShardKey0: "jumped over", myShardKey1: "the lazy dog", _id: 1234567890}
        }]
    }));
    session.commitTransaction();

    const containerEntriesAfterInsert = entriesInContainer(primary, globalIndexUUID);
    assert.eq(containerEntriesAfterInsert, containerEntriesBefore + 1);

    // We can't reinsert that same key, as that'll trigger a duplicate key error.
    session.startTransaction();
    assert.commandFailedWithCode(session.getDatabase("test").runCommand({
        _shardsvrWriteGlobalIndexKeys: 1,
        ops: [{
            _shardsvrInsertGlobalIndexKey: globalIndexUUID,
            key: {myKey: "the quick brown fox"},
            docKey: {myShardKey0: "another", myShardKey1: "value", _id: 0}
        }]
    }),
                                 ErrorCodes.DuplicateKey);
    session.abortTransaction();

    // Update the key.
    session.startTransaction();
    assert.commandWorked(session.getDatabase("test").runCommand({
        _shardsvrWriteGlobalIndexKeys: 1,
        ops: [
            {
                _shardsvrDeleteGlobalIndexKey: globalIndexUUID,
                key: {myKey: "the quick brown fox"},
                docKey: {myShardKey0: "jumped over", myShardKey1: "the lazy dog", _id: 1234567890}
            },
            {
                _shardsvrInsertGlobalIndexKey: globalIndexUUID,
                key: {myKey: "The Quick Brown Fox"},
                docKey: {myShardKey0: "jumped over", myShardKey1: "the lazy dog", _id: 1234567890}
            }
        ]
    }));
    session.commitTransaction();

    const containerEntriesAfterUpdate = entriesInContainer(primary, globalIndexUUID);
    assert.eq(containerEntriesAfterInsert, containerEntriesAfterUpdate);

    // Verify that the index key update succeeded: we can re-insert the old key, we get a duplicate
    // key error if reinsterting the updated key.
    session.startTransaction();
    assert.commandWorked(session.getDatabase("test").runCommand({
        _shardsvrWriteGlobalIndexKeys: 1,
        ops: [
            {
                _shardsvrInsertGlobalIndexKey: globalIndexUUID,
                key: {myKey: "the quick brown fox"},
                docKey: {myShardKey0: "another", myShardKey1: "shardKeyValue", _id: 0}
            },
        ]
    }));
    session.commitTransaction();
    session.startTransaction();
    assert.commandFailedWithCode(session.getDatabase("test").runCommand({
        _shardsvrWriteGlobalIndexKeys: 1,
        ops: [{
            _shardsvrInsertGlobalIndexKey: globalIndexUUID,
            key: {myKey: "The Quick Brown Fox"},
            docKey: {myShardKey0: "jumped over", myShardKey1: "the lazy dog", _id: 1234567890}
        }]
    }),
                                 ErrorCodes.DuplicateKey);
    session.abortTransaction();
}

// A duplicate key error rolls back the whole bulk write.
{
    const containerEntriesBefore = entriesInContainer(primary, globalIndexUUID);

    session.startTransaction();
    assert.commandWorked(session.getDatabase("test").runCommand({
        _shardsvrWriteGlobalIndexKeys: 1,
        ops: [{
            _shardsvrInsertGlobalIndexKey: globalIndexUUID,
            key: {myKey: "testDupes"},
            docKey: {myShardKey0: "test", _id: "dupes"}
        }]
    }));
    session.commitTransaction();

    const containerEntriesAfterInsert = entriesInContainer(primary, globalIndexUUID);
    assert.eq(containerEntriesAfterInsert, containerEntriesBefore + 1);

    session.startTransaction();
    assert.commandFailedWithCode(session.getDatabase("test").runCommand({
        _shardsvrWriteGlobalIndexKeys: 1,
        ops: [
            {
                _shardsvrInsertGlobalIndexKey: globalIndexUUID,
                key: {myKey: "new entry"},
                docKey: {myShardKey0: "new", _id: "entry"}
            },
            {
                _shardsvrInsertGlobalIndexKey: globalIndexUUID,
                key: {myKey: "testDupes"},
                docKey: {myShardKey0: "test", _id: "dupes"}
            }
        ]
    }),
                                 ErrorCodes.DuplicateKey);
    session.abortTransaction();

    const containerEntriesAfterDupKeyError = entriesInContainer(primary, globalIndexUUID);
    assert.eq(containerEntriesAfterInsert, containerEntriesAfterDupKeyError);
}

// A KeyNotFound error rolls back the whole bulk write.
{
    const containerEntriesBefore = entriesInContainer(primary, globalIndexUUID);

    session.startTransaction();
    assert.commandFailedWithCode(session.getDatabase("test").runCommand({
        _shardsvrWriteGlobalIndexKeys: 1,
        ops: [
            {
                _shardsvrInsertGlobalIndexKey: globalIndexUUID,
                key: {myKey: "key that will be rolled back"},
                docKey: {myShardKey0: "key", _id: "roll back"}
            },
            {
                _shardsvrDeleteGlobalIndexKey: globalIndexUUID,
                key: {myKey: "key that doesn't exist"},
                docKey: {myShardKey0: "does not", _id: "exist"}
            }
        ]
    }),
                                 ErrorCodes.KeyNotFound);
    session.abortTransaction();

    const containerEntriesAfter = entriesInContainer(primary, globalIndexUUID);
    assert.eq(containerEntriesBefore, containerEntriesAfter);
}

// A KeyNotFound error on the docKey rolls back the whole bulk write.
{
    const containerEntriesBefore = entriesInContainer(primary, globalIndexUUID);

    session.startTransaction();
    assert.commandFailedWithCode(session.getDatabase("test").runCommand({
        _shardsvrWriteGlobalIndexKeys: 1,
        ops: [
            {
                _shardsvrInsertGlobalIndexKey: globalIndexUUID,
                key: {myKey: "key that will be rolled back"},
                docKey: {myShardKey0: "key", _id: "roll back"}
            },
            {
                _shardsvrDeleteGlobalIndexKey: globalIndexUUID,
                key: {myKey: "key that will be rolled back"},
                docKey: {myShardKey0: "docKey", _id: "doesn't match"}
            }
        ]
    }),
                                 ErrorCodes.KeyNotFound);
    session.abortTransaction();

    const containerEntriesAfter = entriesInContainer(primary, globalIndexUUID);
    assert.eq(containerEntriesBefore, containerEntriesAfter);
}

// Insert 2k index entries in bulk.
{
    const containerEntriesBefore = entriesInContainer(primary, globalIndexUUID);
    let ops = [];
    for (let i = 0; i < 2000; i++) {
        ops.push(
            {_shardsvrInsertGlobalIndexKey: globalIndexUUID, key: {a: i}, docKey: {sk: i, _id: i}});
    }

    session.startTransaction();
    assert.commandWorked(
        session.getDatabase("test").runCommand({_shardsvrWriteGlobalIndexKeys: 1, ops: ops}));
    session.commitTransaction();

    const containerEntriesAfter = entriesInContainer(primary, globalIndexUUID);
    assert.eq(containerEntriesAfter, containerEntriesBefore + 2000);
}

// Delete the 2k index entries above in bulk.
{
    const containerEntriesBefore = entriesInContainer(primary, globalIndexUUID);

    let ops = [];
    for (let i = 0; i < 2000; i++) {
        ops.push(
            {_shardsvrDeleteGlobalIndexKey: globalIndexUUID, key: {a: i}, docKey: {sk: i, _id: i}});
    }

    session.startTransaction();
    assert.commandWorked(
        session.getDatabase("test").runCommand({_shardsvrWriteGlobalIndexKeys: 1, ops: ops}));
    session.commitTransaction();

    const containerEntriesAfter = entriesInContainer(primary, globalIndexUUID);
    assert.eq(containerEntriesAfter, containerEntriesBefore - 2000);
}

// Insert and delete the same 2k entries in the same bulk.
{
    const containerEntriesBefore = entriesInContainer(primary, globalIndexUUID);
    let ops = [];
    for (let i = 0; i < 2000; i++) {
        ops.push(
            {_shardsvrInsertGlobalIndexKey: globalIndexUUID, key: {a: i}, docKey: {sk: i, _id: i}});
        ops.push(
            {_shardsvrDeleteGlobalIndexKey: globalIndexUUID, key: {a: i}, docKey: {sk: i, _id: i}});
    }

    session.startTransaction();
    assert.commandWorked(
        session.getDatabase("test").runCommand({_shardsvrWriteGlobalIndexKeys: 1, ops: ops}));
    session.commitTransaction();

    const containerEntriesAfter = entriesInContainer(primary, globalIndexUUID);
    assert.eq(containerEntriesAfter, containerEntriesBefore);
}

// Insert and delete keys on multiple global index containers.
{
    const otherGlobalIndexUUID = UUID();
    assert.commandWorked(adminDB.runCommand({_shardsvrCreateGlobalIndex: otherGlobalIndexUUID}));
    assert.eq(0, entriesInContainer(primary, otherGlobalIndexUUID));

    const containerEntriesBefore = entriesInContainer(primary, globalIndexUUID);
    const otherContainerEntriesBefore = entriesInContainer(primary, otherGlobalIndexUUID);

    session.startTransaction();
    assert.commandWorked(session.getDatabase("test").runCommand({
        _shardsvrWriteGlobalIndexKeys: 1,
        ops: [
            {
                _shardsvrInsertGlobalIndexKey: globalIndexUUID,
                key: {k0: "first"},
                docKey: {sk0: "first", _id: "first"}
            },
            {
                _shardsvrInsertGlobalIndexKey: otherGlobalIndexUUID,
                key: {k0: "firstOnSecondContainer"},
                docKey: {sk0: "firstOnSecondContainer", _id: "firstOnSecondContainer"}
            },
            {
                _shardsvrInsertGlobalIndexKey: otherGlobalIndexUUID,
                key: {k0: "secondOnSecondContainer"},
                docKey: {sk0: "secondOnSecondContainer", _id: "secondOnSecondContainer"}
            },
            {
                _shardsvrDeleteGlobalIndexKey: globalIndexUUID,
                key: {k0: "first"},
                docKey: {sk0: "first", _id: "first"}
            },
            {
                _shardsvrInsertGlobalIndexKey: globalIndexUUID,
                key: {k0: "firstUpdated"},
                docKey: {sk0: "first", _id: "first"}
            },
            {
                _shardsvrDeleteGlobalIndexKey: otherGlobalIndexUUID,
                key: {k0: "firstOnSecondContainer"},
                docKey: {sk0: "firstOnSecondContainer", _id: "firstOnSecondContainer"}
            },
        ]
    }));
    session.commitTransaction();

    const containerEntriesAfter = entriesInContainer(primary, globalIndexUUID);
    const otherContainerEntriesAfter = entriesInContainer(primary, otherGlobalIndexUUID);
    assert.eq(containerEntriesAfter, containerEntriesBefore + 1);
    assert.eq(otherContainerEntriesAfter, otherContainerEntriesBefore + 1);

    // Verify that that the expected index keys are in place by fetching by document key, and by
    // triggering DuplicateKey error on inserting the same index key.
    assert.eq(1,
              primary.getDB("system")
                  .getCollection("globalIndex." + extractUUIDFromObject(globalIndexUUID))
                  .find({_id: {sk0: "first", _id: "first"}})
                  .itcount());
    session.startTransaction();
    assert.commandFailedWithCode(session.getDatabase("test").runCommand({
        _shardsvrInsertGlobalIndexKey: globalIndexUUID,
        key: {k0: "firstUpdated"},
        docKey: {sk0: "doesn't", _id: "matter"}
    }),
                                 ErrorCodes.DuplicateKey);
    session.abortTransaction();
    assert.eq(1,
              primary.getDB("system")
                  .getCollection("globalIndex." + extractUUIDFromObject(otherGlobalIndexUUID))
                  .find({_id: {sk0: "secondOnSecondContainer", _id: "secondOnSecondContainer"}})
                  .itcount());
    session.startTransaction();
    assert.commandFailedWithCode(session.getDatabase("test").runCommand({
        _shardsvrInsertGlobalIndexKey: otherGlobalIndexUUID,
        key: {k0: "secondOnSecondContainer"},
        docKey: {sk0: "doesn't", _id: "matter"}
    }),
                                 ErrorCodes.DuplicateKey);
    session.abortTransaction();
}

// Insert and delete keys in bulk while inserting a document to a collection in the same multi-doc
// transaction.
{
    const containerEntriesBefore = entriesInContainer(primary, globalIndexUUID);

    session.startTransaction();
    assert.commandWorked(session.getDatabase("test").runCommand({
        _shardsvrWriteGlobalIndexKeys: 1,
        ops: [
            {
                _shardsvrInsertGlobalIndexKey: globalIndexUUID,
                key: {k0: "globalIndexKey"},
                docKey: {sk0: "globalIndexKey", _id: "globalIndexKey"}
            },
            {
                _shardsvrInsertGlobalIndexKey: globalIndexUUID,
                key: {k0: "globalIndexKey2"},
                docKey: {sk0: "globalIndexKey2", _id: "globalIndexKey"}
            },
        ]
    }));
    assert.commandWorked(session.getDatabase("test").getCollection("c").insertOne({x: "data"}));
    session.commitTransaction();

    const containerEntriesAfter = entriesInContainer(primary, globalIndexUUID);
    assert.eq(containerEntriesAfter, containerEntriesBefore + 2);

    // Verify that that the expected index keys are in place by fetching by document key, and by
    // triggering DuplicateKey error on inserting the same index key.
    assert.eq(1,
              primary.getDB("system")
                  .getCollection("globalIndex." + extractUUIDFromObject(globalIndexUUID))
                  .find({_id: {sk0: "globalIndexKey", _id: "globalIndexKey"}})
                  .itcount());
    assert.eq(1,
              primary.getDB("system")
                  .getCollection("globalIndex." + extractUUIDFromObject(globalIndexUUID))
                  .find({_id: {sk0: "globalIndexKey2", _id: "globalIndexKey"}})
                  .itcount());
    session.startTransaction();
    assert.commandFailedWithCode(session.getDatabase("test").runCommand({
        _shardsvrInsertGlobalIndexKey: globalIndexUUID,
        key: {k0: "globalIndexKey"},
        docKey: {sk0: "doesn't", _id: "matter"}
    }),
                                 ErrorCodes.DuplicateKey);
    session.abortTransaction();
    session.startTransaction();
    assert.commandFailedWithCode(session.getDatabase("test").runCommand({
        _shardsvrInsertGlobalIndexKey: globalIndexUUID,
        key: {k0: "globalIndexKey2"},
        docKey: {sk0: "doesn't", _id: "matter"}
    }),
                                 ErrorCodes.DuplicateKey);
    session.abortTransaction();
    assert.eq(1, primary.getDB("test").getCollection("c").find({x: "data"}).itcount());
}

// A bulk write broken down into multiple applyOps.
{
    const uuid = UUID();
    // Generate three applyOps entries, the last one only containing a single statement.
    const stmts = 2 * maxNumberOfTxnOpsInSingleOplogEntry + 1;
    assert.commandWorked(adminDB.runCommand({_shardsvrCreateGlobalIndex: uuid}));
    assert.eq(0, entriesInContainer(primary, uuid));

    let ops = [];
    for (let i = 0; i < stmts; i++) {
        ops.push({_shardsvrInsertGlobalIndexKey: uuid, key: {a: i}, docKey: {sk: i, _id: i}});
    }
    session.startTransaction();
    assert.commandWorked(
        session.getDatabase("test").runCommand({_shardsvrWriteGlobalIndexKeys: 1, ops: ops}));
    session.commitTransaction();

    // The global index container consists of 'stmts' entries.
    assert.eq(stmts, entriesInContainer(primary, uuid));

    // The transaction is split into three applyOps entries, with 'stmts' entries in total.
    const applyOpsSummary =
        adminDB.getSiblingDB('local')
            .oplog.rs
            .aggregate([
                {$match: {ns: "admin.$cmd", 'o.applyOps.op': 'xi', 'o.applyOps.ui': uuid}},
                {$project: {count: {$size: '$o.applyOps'}}},
            ])
            .toArray();
    assert.eq(3, applyOpsSummary.length);
    assert.eq(maxNumberOfTxnOpsInSingleOplogEntry, applyOpsSummary[0]["count"]);
    assert.eq(maxNumberOfTxnOpsInSingleOplogEntry, applyOpsSummary[1]["count"]);
    assert.eq(1, applyOpsSummary[2]["count"]);
    assert.eq(
        stmts,
        applyOpsSummary[0]["count"] + applyOpsSummary[1]["count"] + applyOpsSummary[2]["count"]);
}

// Global index CRUD ops generate the same oplog entry, regardless of whether the transaction
// statements are bulked together. Also validates the oplog entry.
{
    {
        const containerEntriesBefore = entriesInContainer(primary, globalIndexUUID);
        session.startTransaction();
        assert.commandWorked(session.getDatabase("test").runCommand({
            _shardsvrWriteGlobalIndexKeys: 1,
            ops: [
                {
                    _shardsvrInsertGlobalIndexKey: globalIndexUUID,
                    key: {myKey: "insertAndRemove"},
                    docKey: {shardKey: "insert", _id: "andRemove"}
                },
                {
                    _shardsvrDeleteGlobalIndexKey: globalIndexUUID,
                    key: {myKey: "insertAndRemove"},
                    docKey: {shardKey: "insert", _id: "andRemove"}
                }
            ]
        }));
        session.commitTransaction();
        const containerEntriesAfter = entriesInContainer(primary, globalIndexUUID);
        assert.eq(containerEntriesAfter, containerEntriesBefore);
    }
    {
        const containerEntriesBefore = entriesInContainer(primary, globalIndexUUID);
        session.startTransaction();
        assert.commandWorked(session.getDatabase("test").runCommand({
            _shardsvrInsertGlobalIndexKey: globalIndexUUID,
            key: {myKey: "insertAndRemove"},
            docKey: {shardKey: "insert", _id: "andRemove"}
        }));
        assert.commandWorked(session.getDatabase("test").runCommand({
            _shardsvrDeleteGlobalIndexKey: globalIndexUUID,
            key: {myKey: "insertAndRemove"},
            docKey: {shardKey: "insert", _id: "andRemove"}
        }));
        session.commitTransaction();
        const containerEntriesAfter = entriesInContainer(primary, globalIndexUUID);
        assert.eq(containerEntriesAfter, containerEntriesBefore);
    }

    const oplogEntries =
        adminDB.getSiblingDB('local')
            .oplog.rs
            .find({ns: "admin.$cmd", 'o.applyOps.op': 'xi', 'o.applyOps.ui': globalIndexUUID})
            .sort({$natural: -1})
            .limit(2)
            .toArray();

    let oplogEntryBulk = oplogEntries[0];
    let oplogEntryPlain = oplogEntries[1];
    assert.neq(oplogEntryBulk["txnNumber"], oplogEntryPlain["txnNumber"]);
    assert.neq(oplogEntryBulk["ts"], oplogEntryPlain["ts"]);
    assert.neq(oplogEntryBulk["wall"], oplogEntryPlain["wall"]);
    delete oplogEntryBulk.ts;
    delete oplogEntryPlain.ts;
    delete oplogEntryBulk.txnNumber;
    delete oplogEntryPlain.txnNumber;
    delete oplogEntryBulk.wall;
    delete oplogEntryPlain.wall;
    assert.docEq(oplogEntryBulk, oplogEntryPlain);
    assert.eq(oplogEntryBulk["o"]["applyOps"][0]["op"], "xi");
    assert.docEq(oplogEntryBulk["o"]["applyOps"][0]["o"]["ik"], {myKey: "insertAndRemove"});
    assert.docEq(oplogEntryBulk["o"]["applyOps"][0]["o"]["dk"],
                 {shardKey: "insert", _id: "andRemove"});
    assert.eq(oplogEntryBulk["o"]["applyOps"][1]["op"], "xd");
    assert.docEq(oplogEntryBulk["o"]["applyOps"][1]["o"]["ik"], {myKey: "insertAndRemove"});
    assert.docEq(oplogEntryBulk["o"]["applyOps"][1]["o"]["dk"],
                 {shardKey: "insert", _id: "andRemove"});
}

session.endSession();
rst.stopSet();
})();
