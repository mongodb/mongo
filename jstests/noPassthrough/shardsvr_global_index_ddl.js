/**
 * Tests global index ddl operations, _shardsvrCreateGlobalIndex and _shardsvrDropGlobalIndex.
 *
 * @tags: [
 *     featureFlagGlobalIndexes,
 *     requires_fcv_62,
 *     requires_replication,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/collection_drop_recreate.js");  // For assertDropCollection.

function uuidToString(uuid) {
    const [_, uuidString] = uuid.toString().match(/"((?:\\.|[^"\\])*)"/);
    return uuidString;
}

function verifyCollectionExists(node, globalIndexUUID, namespace) {
    const systemDB = node.getDB("system");
    const res = systemDB.runCommand({listCollections: 1, filter: {name: namespace}});
    assert.eq(res.cursor.firstBatch.length, 1);
    assert.eq(res.cursor.firstBatch[0].info.uuid, globalIndexUUID);
}

function verifyCollectionNotFound(node, namespace) {
    const systemDB = node.getDB("system");
    const res = systemDB.runCommand({listCollections: 1, filter: {name: namespace}});
    assert.eq(res.cursor.firstBatch.length, 0);
}

function verifyIndexSpecs(node, namespace) {
    const primaryIndexSpecs =
        {"v": 2, "key": {"_id": 1}, "name": "_id_", "unique": true, "clustered": true};

    const secondaryIndexSpecs = {"v": 2, "key": {"ik": 1}, "name": "ik_1", "unique": true};

    const referenceIndexSpecList = [primaryIndexSpecs, secondaryIndexSpecs];

    const systemDB = node.getDB("system");
    let listIndexes = systemDB.runCommand({listIndexes: namespace});
    assert.commandWorked(listIndexes);

    const indexSpecList = listIndexes["cursor"]["firstBatch"];
    assert.sameMembers(indexSpecList,
                       referenceIndexSpecList,
                       "Global index collection has unexpected index specs.");
}

function verifyMultiDocumentTransactionDisallowed(node, commandObj) {
    let session = node.startSession();
    // Verify the command is not allowed in multi-document transactions.
    let sessionDB = session.getDatabase("admin");
    session.startTransaction();

    assert.commandFailedWithCode(sessionDB.runCommand(commandObj),
                                 ErrorCodes.OperationNotSupportedInTransaction);
    session.endSession();
}

function verifyOplogEntry(node, globalIndexUUID, namespace, commandString, lsid, txnNumber) {
    // Sample oplog entry.
    // {
    // 	"op" : "c",
    // 	"ns" : "system.$cmd",
    // 	"ui" : UUID("abe869a0-932f-418c-9baa-2f826fbf23e9"),
    // 	"o" : {
    // 		"createGlobalIndex" : "globalIndexes.abe869a0-932f-418c-9baa-2f826fbf23e9"
    // 	},
    // 	"ts" : Timestamp(1659625616, 4),
    // 	"t" : NumberLong(1),
    // 	"v" : NumberLong(2),
    // 	"wall" : ISODate("2022-08-04T15:06:56.647Z")
    // }
    var commandStringNamespaceObj = {};
    commandStringNamespaceObj[commandString] = namespace;

    const oplogEntry = node.getDB("local").oplog.rs.find({}).sort({$natural: -1}).limit(1).next();
    assert.eq(oplogEntry.op, "c");
    assert.eq(oplogEntry.ui, globalIndexUUID);
    assert.docEq(oplogEntry.o, commandStringNamespaceObj);

    // lsid and txnNumber are either both present (retryable writes) or absent.
    assert((lsid && txnNumber) || (!lsid && !txnNumber));
    if (lsid) {
        assert.eq(oplogEntry.lsid.id, lsid.id);
        assert.eq(oplogEntry.txnNumber, txnNumber);
        assert.eq(oplogEntry.stmtId, 0);
    }
}

function verifyCommandIsRetryableWrite(node, command, oplogCommandString, setup) {
    const session = node.startSession({retryWrites: true});
    const adminDB = session.getDatabase("admin");
    const lsid = session.getSessionId();
    const txnNumber = NumberLong(10);
    const indexUUID = UUID();
    const globalIndexCollName = "globalIndexes." + uuidToString(indexUUID);

    var commandObj = {};
    commandObj[command] = indexUUID;
    commandObj = Object.assign(commandObj, {lsid: lsid, txnNumber: txnNumber});

    const commandInvocations = 5;
    const ssBefore = assert.commandWorked(node.getDB("test").runCommand({serverStatus: 1}));

    const retriedCommandsCountBefore = ssBefore["transactions"]["retriedCommandsCount"];
    const retriedStatementsCountBefore = ssBefore["transactions"]["retriedStatementsCount"];
    const transactionsCollectionWriteCountBefore =
        ssBefore["transactions"]["transactionsCollectionWriteCount"];
    const commandCountBefore = ssBefore["metrics"]["commands"][command]["total"];

    if (typeof setup !== "undefined") {
        setup(adminDB, indexUUID);
    }

    const doRetry = () => {
        assert.commandWorked(adminDB.runCommand(commandObj));
    };

    // Run the same retryable _shardsvrCreateGlobalIndex invocation multiple times.
    for (var i = 0; i < commandInvocations; i++) {
        doRetry();
    }

    const ssAfter = assert.commandWorked(node.getDB("test").runCommand({serverStatus: 1}));
    const retriedCommandsCountAfter = ssAfter["transactions"]["retriedCommandsCount"];
    const retriedStatementsCountAfter = ssAfter["transactions"]["retriedStatementsCount"];
    const transactionsCollectionWriteCountAfter =
        ssAfter["transactions"]["transactionsCollectionWriteCount"];
    const commandCountAfter = ssAfter["metrics"]["commands"][command]["total"];

    // The increase of retried commands and statements are globally >= the number of command
    // invocations.
    assert.gte(retriedCommandsCountAfter, retriedCommandsCountBefore + commandInvocations - 1);
    assert.gte(retriedStatementsCountAfter, retriedStatementsCountBefore + commandInvocations - 1);
    // The command executed exactly once (config.transactions are globally >= 1) while the command
    // was invoked more than once.
    assert.gte(transactionsCollectionWriteCountAfter, transactionsCollectionWriteCountBefore + 1);
    assert.gte(commandCountAfter, commandCountBefore + commandInvocations);

    verifyOplogEntry(node, indexUUID, globalIndexCollName, oplogCommandString, lsid, txnNumber);
}

function testCreateGlobalIndex(rst) {
    const primary = rst.getPrimary();
    const adminDB = primary.getDB("admin");
    const globalIndexUUID = UUID();
    const globalIndexCollName = "globalIndexes." + uuidToString(globalIndexUUID);
    const oplogCommandString = "createGlobalIndex";

    verifyMultiDocumentTransactionDisallowed(primary, {_shardsvrCreateGlobalIndex: UUID()});
    verifyCommandIsRetryableWrite(primary, "_shardsvrCreateGlobalIndex", oplogCommandString);

    // Create a global index container.
    assert.commandWorked(adminDB.runCommand({_shardsvrCreateGlobalIndex: globalIndexUUID}));
    // The command is idempotent: it returns OK if the container already exists.
    assert.commandWorked(adminDB.runCommand({_shardsvrCreateGlobalIndex: globalIndexUUID}));
    verifyOplogEntry(primary, globalIndexUUID, globalIndexCollName, oplogCommandString);

    rst.awaitReplication();
    rst.nodes.forEach((node) => {
        // Verify collection exists in all nodes, command does what is expected in primary and is
        // replicated to secondaries.
        verifyCollectionExists(node, globalIndexUUID, globalIndexCollName);
        // Verify index spec in all nodes.
        verifyIndexSpecs(node, globalIndexCollName);
    });
}

function testDropGlobalIndex(rst) {
    const primary = rst.getPrimary();
    const adminDB = primary.getDB("admin");
    const globalIndexUUID = UUID();
    const globalIndexCollName = "globalIndexes." + uuidToString(globalIndexUUID);
    const oplogCommandString = "dropGlobalIndex";

    verifyMultiDocumentTransactionDisallowed(primary, {_shardsvrDropGlobalIndex: UUID()});

    const retryableWriteTestSetup = function(db, uuid) {
        // Create a global index container to be deleted.
        assert.commandWorked(db.runCommand({_shardsvrCreateGlobalIndex: uuid}));
    };
    verifyCommandIsRetryableWrite(
        primary, "_shardsvrDropGlobalIndex", oplogCommandString, retryableWriteTestSetup);

    // Create a global index container to be deleted.
    assert.commandWorked(adminDB.runCommand({_shardsvrCreateGlobalIndex: globalIndexUUID}));

    assert.commandWorked(adminDB.runCommand({_shardsvrDropGlobalIndex: globalIndexUUID}));

    verifyOplogEntry(primary, globalIndexUUID, globalIndexCollName, oplogCommandString);

    // The command is idempotent: it returns OK if the container does not exist.
    assert.commandWorked(adminDB.runCommand({_shardsvrDropGlobalIndex: globalIndexUUID}));

    rst.awaitReplication();
    rst.nodes.forEach((node) => {
        // Verify collection non-existing in all nodes.
        verifyCollectionNotFound(node, globalIndexCollName);
    });
}

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

testCreateGlobalIndex(rst);
testDropGlobalIndex(rst);

rst.stopSet();
})();
