/**
 * Tests replication rollback of global index container DDL and CRUD operations.
 * Validates the generation of rollback files and efficient restoring of fast-counts.
 *
 * @tags: [
 *     featureFlagGlobalIndexes,
 *     requires_fcv_62,
 *     requires_replication,
 * ]
 */

(function() {
'use strict';

load('jstests/replsets/libs/rollback_files.js');
load('jstests/replsets/libs/rollback_test.js');
load('jstests/libs/uuid_util.js');

function uuidToCollName(uuid) {
    return "globalIndex." + extractUUIDFromObject(uuid);
}

const rollbackTest = new RollbackTest(jsTestName());

function rollbackDDLOps() {
    const node = rollbackTest.getPrimary();
    const adminDB = node.getDB("admin");
    const globalIndexCreateUUID = UUID();
    const globalIndexDropUUID = UUID();
    jsTestLog("rollbackDDLOps primary=" + node);

    // Create a global index container whose drop won't be majority-committed.
    assert.commandWorked(adminDB.runCommand({_shardsvrCreateGlobalIndex: globalIndexDropUUID}));

    rollbackTest.transitionToRollbackOperations();

    // Create a global index container that's not majority-committed.
    assert.commandWorked(adminDB.runCommand({_shardsvrCreateGlobalIndex: globalIndexCreateUUID}));
    // Drop a global index container, the operation is not majority-committed.
    assert.commandWorked(adminDB.runCommand({_shardsvrDropGlobalIndex: globalIndexDropUUID}));

    // Perform the rollback.
    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    rollbackTest.transitionToSteadyStateOperations();

    // Check globalIndexCreateUUID creation is rolled back and does not exist.
    var res = node.getDB("system").runCommand(
        {listCollections: 1, filter: {name: uuidToCollName(globalIndexCreateUUID)}});
    assert.eq(res.cursor.firstBatch.length, 0);

    // Check globalIndexDropUUID drop is rolled back and still exists.
    res = node.getDB("system").runCommand(
        {listCollections: 1, filter: {name: uuidToCollName(globalIndexDropUUID)}});
    assert.eq(res.cursor.firstBatch.length, 1);

    // Log calls out that the two commands have been rolled back.
    assert(checkLog.checkContainsWithCountJson(
        node,
        21656,
        {
            "oplogEntry": {
                "op": "c",
                "ns": "system.$cmd",
                "ui": {"$uuid": extractUUIDFromObject(globalIndexDropUUID)},
                "o": {"dropGlobalIndex": uuidToCollName(globalIndexDropUUID)},
                "o2": {"numRecords": 0}
            }
        },
        1,
        null,
        true /*isRelaxed*/));
    assert(checkLog.checkContainsWithCountJson(
        node,
        21656,
        {
            "oplogEntry": {
                "op": "c",
                "ns": "system.$cmd",
                "ui": {"$uuid": extractUUIDFromObject(globalIndexCreateUUID)},
                "o": {"createGlobalIndex": uuidToCollName(globalIndexCreateUUID)}
            }
        },
        1,
        null,
        true /*isRelaxed*/));
}

// Rollback a single index key insert.
function rollbackSingleKeyInsert(bulk) {
    const node = rollbackTest.getPrimary();
    const adminDB = node.getDB("admin");
    const uuid = UUID();
    jsTestLog("rollbackSingleKeyInsert uuid=" + uuid + ", bulk=" + bulk, ", primary=" + node);

    const collName = uuidToCollName(uuid);
    assert.commandWorked(adminDB.runCommand({_shardsvrCreateGlobalIndex: uuid}));
    assert.eq(0, node.getDB("system").getCollection(collName).find().itcount());

    const keyMajorityCommitted = {key: {a: 0}, docKey: {sk: 0, _id: 0}};
    const keyToRollback = {key: {a: 1}, docKey: {sk: 1, _id: 1}};

    // Insert a key, majority-committed.
    {
        const session = node.startSession();
        session.startTransaction();
        assert.commandWorked(session.getDatabase("system").runCommand(
            Object.extend({"_shardsvrInsertGlobalIndexKey": uuid}, keyMajorityCommitted)));
        session.commitTransaction();
        session.endSession();
    }
    assert.eq(1, node.getDB("system").getCollection(collName).find().itcount());
    assert.eq(1,
              node.getDB("system")
                  .getCollection(collName)
                  .find({_id: keyMajorityCommitted["docKey"]})
                  .itcount());

    // Then insert a key that's not majority-committed.
    rollbackTest.transitionToRollbackOperations();
    {
        const session = node.startSession();
        session.startTransaction();
        const stmts = [Object.extend({"_shardsvrInsertGlobalIndexKey": uuid}, keyToRollback)];
        if (bulk) {
            assert.commandWorked(session.getDatabase("system").runCommand(
                {"_shardsvrWriteGlobalIndexKeys": 1, ops: stmts}));
        } else {
            for (let stmt of stmts) {
                assert.commandWorked(session.getDatabase("system").runCommand(stmt));
            }
        }
        session.commitTransaction();
        session.endSession();
    }
    assert.eq(2, node.getDB("system").getCollection(collName).find().itcount());
    assert.eq(1,
              node.getDB("system")
                  .getCollection(collName)
                  .find({_id: keyMajorityCommitted["docKey"]})
                  .itcount());
    assert.eq(1,
              node.getDB("system")
                  .getCollection(collName)
                  .find({_id: keyToRollback["docKey"]})
                  .itcount());

    // Perform the rollback.
    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    rollbackTest.transitionToSteadyStateOperations();

    // Only the majority-committed key is left.
    assert.eq(1, node.getDB("system").getCollection(collName).find().itcount());
    assert.eq(1,
              node.getDB("system")
                  .getCollection(collName)
                  .find({_id: keyMajorityCommitted["docKey"]})
                  .itcount());
    assert.eq(0,
              node.getDB("system")
                  .getCollection(collName)
                  .find({_id: keyToRollback["docKey"]})
                  .itcount());

    // Log calls out that the index key insert has been rolled back.
    assert(
        checkLog.checkContainsWithCountJson(node,
                                            6984700,
                                            {"insertGlobalIndexKey": 1, "deleteGlobalIndexKey": 0},
                                            1,
                                            null,
                                            true /*isRelaxed*/));

    // The rollback wrote the rolled-back index key insert to a file.
    const replTest = rollbackTest.getTestFixture();
    const expectedEntries = [Object.extend({_id: keyToRollback.docKey},
                                           {"ik": BinData(0, "KwIE"), "tb": BinData(0, "AQ==")})];
    checkRollbackFiles(replTest.getDbPath(node), "system." + collName, uuid, expectedEntries);
}

// Rollback a single index key delete.
function rollbackSingleKeyDelete(bulk) {
    const node = rollbackTest.getPrimary();
    const adminDB = node.getDB("admin");
    const uuid = UUID();
    jsTestLog("rollbackSingleKeyDelete uuid=" + uuid + ", bulk=" + bulk, ", primary=" + node);

    const collName = uuidToCollName(uuid);
    assert.commandWorked(adminDB.runCommand({_shardsvrCreateGlobalIndex: uuid}));
    assert.eq(0, node.getDB("system").getCollection(collName).find().itcount());

    const key = {key: {a: 0}, docKey: {sk: 0, _id: 0}};

    // Insert a key, majority-committed.
    {
        const session = node.startSession();
        session.startTransaction();
        assert.commandWorked(session.getDatabase("system").runCommand(
            Object.extend({"_shardsvrInsertGlobalIndexKey": uuid}, key)));
        session.commitTransaction();
        session.endSession();
    }
    assert.eq(1, node.getDB("system").getCollection(collName).find().itcount());
    assert.eq(1, node.getDB("system").getCollection(collName).find({_id: key["docKey"]}).itcount());

    // Then delete the key, not majority-committed.
    rollbackTest.transitionToRollbackOperations();
    {
        const session = node.startSession();
        session.startTransaction();
        const stmts = [Object.extend({"_shardsvrDeleteGlobalIndexKey": uuid}, key)];
        if (bulk) {
            assert.commandWorked(session.getDatabase("system").runCommand(
                {"_shardsvrWriteGlobalIndexKeys": 1, ops: stmts}));
        } else {
            for (let stmt of stmts) {
                assert.commandWorked(session.getDatabase("system").runCommand(stmt));
            }
        }
        session.commitTransaction();
        session.endSession();
    }
    assert.eq(0, node.getDB("system").getCollection(collName).find().itcount());

    // Perform the rollback.
    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    rollbackTest.transitionToSteadyStateOperations();

    // The key is still present, as its delete wasn't majority-committed.
    assert.eq(1, node.getDB("system").getCollection(collName).find().itcount());
    assert.eq(1, node.getDB("system").getCollection(collName).find({_id: key["docKey"]}).itcount());

    // Log calls out that the index key delete has been rolled back.
    assert(
        checkLog.checkContainsWithCountJson(node,
                                            6984700,
                                            {"insertGlobalIndexKey": 0, "deleteGlobalIndexKey": 1},
                                            1,
                                            null,
                                            true /*isRelaxed*/));
}

function rollbackOneKeyInsertTwoKeyDeletes(bulk) {
    const node = rollbackTest.getPrimary();
    const adminDB = node.getDB("admin");
    const uuid = UUID();
    jsTestLog("rollbackOneKeyInsertTwoKeyDeletes uuid=" + uuid + ", bulk=" + bulk,
              ", primary=" + node);

    const collName = uuidToCollName(uuid);
    assert.commandWorked(adminDB.runCommand({_shardsvrCreateGlobalIndex: uuid}));
    assert.eq(0, node.getDB("system").getCollection(collName).find().itcount());

    const keyMajorityCommitted = {key: {a: 0}, docKey: {sk: 0, _id: 0}};
    const keyToRollback0 = {key: {a: 1}, docKey: {sk: 1, _id: 1}};
    const keyToRollback1 = {key: {a: 2}, docKey: {sk: 2, _id: 2}};

    // Insert a key, majority-committed.
    {
        const session = node.startSession();
        session.startTransaction();
        assert.commandWorked(session.getDatabase("system").runCommand(
            Object.extend({"_shardsvrInsertGlobalIndexKey": uuid}, keyMajorityCommitted)));
        session.commitTransaction();
        session.endSession();
    }
    assert.eq(1, node.getDB("system").getCollection(collName).find().itcount());
    assert.eq(1,
              node.getDB("system")
                  .getCollection(collName)
                  .find({_id: keyMajorityCommitted["docKey"]})
                  .itcount());

    // Then delete the key and insert two more keys. All these writes are not majority-committed.
    rollbackTest.transitionToRollbackOperations();
    {
        const session = node.startSession();
        session.startTransaction();
        const stmts = [
            Object.extend({"_shardsvrDeleteGlobalIndexKey": uuid}, keyMajorityCommitted),
            Object.extend({"_shardsvrInsertGlobalIndexKey": uuid}, keyToRollback0),
            Object.extend({"_shardsvrInsertGlobalIndexKey": uuid}, keyToRollback1)
        ];
        if (bulk) {
            assert.commandWorked(session.getDatabase("system").runCommand(
                {"_shardsvrWriteGlobalIndexKeys": 1, ops: stmts}));
        } else {
            for (let stmt of stmts) {
                assert.commandWorked(session.getDatabase("system").runCommand(stmt));
            }
        }
        session.commitTransaction();
        session.endSession();
    }
    assert.eq(2, node.getDB("system").getCollection(collName).find().itcount());
    assert.eq(1,
              node.getDB("system")
                  .getCollection(collName)
                  .find({_id: keyToRollback0["docKey"]})
                  .itcount());
    assert.eq(1,
              node.getDB("system")
                  .getCollection(collName)
                  .find({_id: keyToRollback1["docKey"]})
                  .itcount());

    // Perform the rollback.
    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    rollbackTest.transitionToSteadyStateOperations();

    // The only key that's present is the majority-committed one.
    assert.eq(1, node.getDB("system").getCollection(collName).find().itcount());
    assert.eq(1,
              node.getDB("system")
                  .getCollection(collName)
                  .find({_id: keyMajorityCommitted["docKey"]})
                  .itcount());

    // Log calls out that two index key inserts and one key delete have been rolled back.
    assert(
        checkLog.checkContainsWithCountJson(node,
                                            6984700,
                                            {"insertGlobalIndexKey": 2, "deleteGlobalIndexKey": 1},
                                            1,
                                            null,
                                            true /*isRelaxed*/));

    // The rollback wrote the two rolled-back index key inserts to a file.
    const replTest = rollbackTest.getTestFixture();
    const expectedEntries = [
        Object.extend({_id: keyToRollback1.docKey},
                      {"ik": BinData(0, "KwQE"), "tb": BinData(0, "AQ==")}),
        Object.extend({_id: keyToRollback0.docKey},
                      {"ik": BinData(0, "KwIE"), "tb": BinData(0, "AQ==")}),
    ];
    checkRollbackFiles(replTest.getDbPath(node), "system." + collName, uuid, expectedEntries);
}

function rollbackCreateWithCrud(bulk) {
    const node = rollbackTest.getPrimary();
    const adminDB = node.getDB("admin");
    const uuid = UUID();
    jsTestLog("rollbackCreateWithCrud uuid=" + uuid + ", bulk=" + bulk, ", primary=" + node);

    const collName = uuidToCollName(uuid);

    const keyToRollback0 = {key: {a: 1}, docKey: {sk: 1, _id: 1}};
    const keyToRollback1 = {key: {a: 2}, docKey: {sk: 2, _id: 2}};
    const keyToRollback2 = {key: {a: 3}, docKey: {sk: 3, _id: 3}};

    // Create a container and insert keys to it. All these operations are not majority-committed.
    rollbackTest.transitionToRollbackOperations();
    {
        assert.commandWorked(adminDB.runCommand({_shardsvrCreateGlobalIndex: uuid}));

        const session = node.startSession();
        session.startTransaction();
        const stmts = [
            Object.extend({"_shardsvrInsertGlobalIndexKey": uuid}, keyToRollback0),
            Object.extend({"_shardsvrInsertGlobalIndexKey": uuid}, keyToRollback1),
            Object.extend({"_shardsvrInsertGlobalIndexKey": uuid}, keyToRollback2),
            Object.extend({"_shardsvrDeleteGlobalIndexKey": uuid}, keyToRollback1),
        ];
        if (bulk) {
            assert.commandWorked(session.getDatabase("system").runCommand(
                {"_shardsvrWriteGlobalIndexKeys": 1, ops: stmts}));
        } else {
            for (let stmt of stmts) {
                assert.commandWorked(session.getDatabase("system").runCommand(stmt));
            }
        }
        session.commitTransaction();
        session.endSession();
    }
    assert.eq(2, node.getDB("system").getCollection(collName).find().itcount());
    assert.eq(1,
              node.getDB("system")
                  .getCollection(collName)
                  .find({_id: keyToRollback0["docKey"]})
                  .itcount());
    assert.eq(0,
              node.getDB("system")
                  .getCollection(collName)
                  .find({_id: keyToRollback1["docKey"]})
                  .itcount());
    assert.eq(1,
              node.getDB("system")
                  .getCollection(collName)
                  .find({_id: keyToRollback2["docKey"]})
                  .itcount());

    // Perform the rollback.
    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    rollbackTest.transitionToSteadyStateOperations();

    // The global index container doesn't exist.
    const container =
        node.getDB("system").runCommand({listCollections: 1, filter: {name: collName}});
    assert.eq(container.cursor.firstBatch.length, 0);

    // Log calls out that three index key inserts and one key delete have been rolled back.
    assert(
        checkLog.checkContainsWithCountJson(node,
                                            6984700,
                                            {"insertGlobalIndexKey": 3, "deleteGlobalIndexKey": 1},
                                            1,
                                            null,
                                            true /*isRelaxed*/));

    // The rollback wrote the two rolled-back index key inserts to a file.
    const replTest = rollbackTest.getTestFixture();
    const expectedEntries = [
        Object.extend({_id: keyToRollback2.docKey},
                      {"ik": BinData(0, "KwYE"), "tb": BinData(0, "AQ==")}),
        Object.extend({_id: keyToRollback0.docKey},
                      {"ik": BinData(0, "KwIE"), "tb": BinData(0, "AQ==")}),
    ];
    checkRollbackFiles(replTest.getDbPath(node), "system." + collName, uuid, expectedEntries);
}

function rollbackDropWithCrud(bulk) {
    const node = rollbackTest.getPrimary();
    const adminDB = node.getDB("admin");
    const uuid = UUID();
    jsTestLog("rollbackDropWithCrud uuid=" + uuid + ", bulk=" + bulk, ", primary=" + node);

    const collName = uuidToCollName(uuid);

    assert.commandWorked(adminDB.runCommand({_shardsvrCreateGlobalIndex: uuid}));
    const keyMajorityCommitted = {key: {a: 0}, docKey: {sk: 0, _id: 0}};

    // Insert a key that will be majority-committed.
    {
        const session = node.startSession();
        session.startTransaction();
        assert.commandWorked(session.getDatabase("system").runCommand(
            Object.extend({"_shardsvrInsertGlobalIndexKey": uuid}, keyMajorityCommitted)));
        session.commitTransaction();
        session.endSession();
    }
    assert.eq(1, node.getDB("system").getCollection(collName).find().itcount());
    assert.eq(1,
              node.getDB("system")
                  .getCollection(collName)
                  .find({_id: keyMajorityCommitted["docKey"]})
                  .itcount());

    const keyToRollback0 = {key: {a: 1}, docKey: {sk: 1, _id: 1}};
    const keyToRollback1 = {key: {a: 2}, docKey: {sk: 2, _id: 2}};
    const keyToRollback2 = {key: {a: 3}, docKey: {sk: 3, _id: 3}};

    // Write to the container and drop it. All these operations are not majority-committed.
    rollbackTest.transitionToRollbackOperations();
    {
        assert.commandWorked(adminDB.runCommand({_shardsvrCreateGlobalIndex: uuid}));

        const session = node.startSession();
        session.startTransaction();
        const stmts = [
            Object.extend({"_shardsvrInsertGlobalIndexKey": uuid}, keyToRollback1),
            Object.extend({"_shardsvrInsertGlobalIndexKey": uuid}, keyToRollback2),
            Object.extend({"_shardsvrDeleteGlobalIndexKey": uuid}, keyToRollback1),
            Object.extend({"_shardsvrInsertGlobalIndexKey": uuid}, keyToRollback0),
            Object.extend({"_shardsvrDeleteGlobalIndexKey": uuid}, keyToRollback2),
        ];
        if (bulk) {
            assert.commandWorked(session.getDatabase("system").runCommand(
                {"_shardsvrWriteGlobalIndexKeys": 1, ops: stmts}));
        } else {
            for (let stmt of stmts) {
                assert.commandWorked(session.getDatabase("system").runCommand(stmt));
            }
        }
        session.commitTransaction();
        session.endSession();
        assert.commandWorked(adminDB.runCommand({_shardsvrDropGlobalIndex: uuid}));
    }
    // The global index container doesn't exist.
    const containerBeforeRollback =
        node.getDB("system").runCommand({listCollections: 1, filter: {name: collName}});
    assert.eq(containerBeforeRollback.cursor.firstBatch.length, 0);

    // Perform the rollback.
    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    rollbackTest.transitionToSteadyStateOperations();

    // The global index exists, with the single majority-committed key.
    const containerAfterRollback =
        node.getDB("system").runCommand({listCollections: 1, filter: {name: collName}});
    assert.eq(containerAfterRollback.cursor.firstBatch.length, 1);

    assert.eq(1, node.getDB("system").getCollection(collName).find().itcount());
    assert.eq(1,
              node.getDB("system")
                  .getCollection(collName)
                  .find({_id: keyMajorityCommitted["docKey"]})
                  .itcount());

    // Log calls out that three index key inserts and two key deletes have been rolled back.
    assert(
        checkLog.checkContainsWithCountJson(node,
                                            6984700,
                                            {"insertGlobalIndexKey": 3, "deleteGlobalIndexKey": 2},
                                            1,
                                            null,
                                            true /*isRelaxed*/));

    // We've reset the original fast count rather than doing an expensive collection scan.
    assert(checkLog.checkContainsWithCountJson(node, 21602, undefined, 0));

    // Log states that we're not going to write a rollback file for a collection whose drop was
    // rolled back.
    assert(checkLog.checkContainsWithCountJson(
        node,
        21608,
        {"uuid": {"uuid": {"$uuid": extractUUIDFromObject(uuid)}}},
        1,
        null,
        true /*isRelaxed*/));
}

rollbackDDLOps();
for (let bulk of [false, true]) {
    rollbackSingleKeyInsert(bulk);
    rollbackSingleKeyDelete(bulk);
    rollbackOneKeyInsertTwoKeyDeletes(bulk);
    rollbackCreateWithCrud(bulk);
    rollbackDropWithCrud(bulk);
}

rollbackTest.stop();
})();
