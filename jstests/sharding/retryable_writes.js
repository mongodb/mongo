/**
 * Test basic retryable write without errors by checking that the resulting collection after the
 * retry is as expected and it does not create additional oplog entries.
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

Random.setRandomSeed();

function dropSessionsCollection(mainConn, priConn, configConn) {
    jsTest.log("Dropping sessions collection.");
    if (mainConn == priConn) {
        // ReplicaSet, just drop the collection on the primary.
        priConn.getDB("config").getCollection("system.sessions").drop();
    } else {
        // Sharded cluster, drop the collection everywhere. Since the collection is sharded we need
        // to clean up not only the local catalog (as above on replica sets) but also the metadata
        // in the global catalog.
        // TODO (SERVER-107023): replace this once we have the ability to drop the sessions
        // collection internally.
        let collDoc = configConn.getDB("config").getCollection("collections").findOne({
            _id: "config.system.sessions",
        });
        if (collDoc == undefined) {
            return;
        }
        let uuid = collDoc.uuid;
        assert.commandWorked(
            configConn
                .getDB("config")
                .getCollection("collections")
                .deleteOne({_id: "config.system.sessions"}, {writeConcern: {w: "majority"}}),
        );
        assert.commandWorked(
            configConn
                .getDB("config")
                .getCollection("chunks")
                .deleteMany({uuid: uuid}, {writeConcern: {w: "majority"}}),
        );
        // We update the placement history here to satisfy the hook which validates this information
        // though in practices these inconsistencies do not matter since you cannot open change
        // streams on config.system.sessions.
        let existingPlacementDoc = configConn
            .getDB("config")
            .getCollection("placementHistory")
            .find()
            .sort({timestamp: -1})
            .limit(1)[0];
        let newPlacement = {
            _id: ObjectId(),
            "nss": "config.system.sessions",
            "uuid": uuid,
            "timestamp": Timestamp(
                existingPlacementDoc.timestamp.getTime(),
                existingPlacementDoc.timestamp.getInc() + 1,
            ),
            "shards": [],
        };
        assert.commandWorked(
            configConn
                .getDB("config")
                .getCollection("placementHistory")
                .insertOne(newPlacement, {
                    writeConcern: {w: "majority"},
                }),
        );
        configConn.getDB("config").getCollection("system.sessions").drop();
        priConn.getDB("config").getCollection("system.sessions").drop();
        assert.commandWorked(configConn.adminCommand({_flushRoutingTableCacheUpdates: "config.system.sessions"}));
        assert.commandWorked(priConn.adminCommand({_flushRoutingTableCacheUpdates: "config.system.sessions"}));
    }
}

function createSessionsCollection(configConn) {
    jsTest.log("Forcing refresh of the sessions collection");
    assert.commandWorked(configConn.adminCommand({refreshLogicalSessionCacheNow: 1}));
}

function checkFindAndModifyResult(expected, toCheck) {
    assert.eq(expected.ok, toCheck.ok);
    assert.eq(expected.value, toCheck.value);
    assert.docEq(expected.lastErrorObject, toCheck.lastErrorObject);
}

function verifyServerStatusFields(serverStatusResponse) {
    assert(
        serverStatusResponse.hasOwnProperty("transactions"),
        "Expected the serverStatus response to have a 'transactions' field",
    );
    assert.hasFields(
        serverStatusResponse.transactions,
        ["retriedCommandsCount", "retriedStatementsCount", "transactionsCollectionWriteCount"],
        "The 'transactions' field in serverStatus did not have all of the expected fields",
    );
}

function verifyServerStatusChanges(initialStats, newStats, newCommands, newStatements, newCollectionWrites) {
    assert.eq(
        initialStats.retriedCommandsCount + newCommands,
        newStats.retriedCommandsCount,
        "expected retriedCommandsCount to increase by " + newCommands,
    );
    assert.eq(
        initialStats.retriedStatementsCount + newStatements,
        newStats.retriedStatementsCount,
        "expected retriedStatementsCount to increase by " + newStatements,
    );
    assert.eq(
        initialStats.transactionsCollectionWriteCount + newCollectionWrites,
        newStats.transactionsCollectionWriteCount,
        "expected retriedCommandsCount to increase by " + newCollectionWrites,
    );
}

function handleSessionsCollection(mainConn, priConn, configConn) {
    // We have to either drop and recreate or do neither because in config shard suites otherwise
    // the periodic creation will mess with the transaction.
    let createdCollection = false;
    if (Math.random() < 0.25) {
        dropSessionsCollection(mainConn, priConn, configConn);
        createSessionsCollection(configConn);
        createdCollection = true;
    }
    // If we are in a config shard and we dropped and recreated the sessions collection then, as
    // part of the create coordinator, we ran a transaction which will mess with server status
    // counts.
    return TestData.configShard && mainConn != priConn && createdCollection;
}

function runTests(mainConn, priConn, configConn) {
    let lsid = UUID();
    assert.commandWorked(mainConn.getDB("test").createCollection("user"));

    ////////////////////////////////////////////////////////////////////////
    // Test insert command

    let initialStatus = priConn.adminCommand({serverStatus: 1});
    verifyServerStatusFields(initialStatus);

    let cmd = {
        insert: "user",
        documents: [{_id: 10}, {_id: 30}],
        ordered: false,
        lsid: {id: lsid},
        txnNumber: NumberLong(34),
    };

    let testDBMain = mainConn.getDB("test");
    let result = assert.commandWorked(testDBMain.runCommand(cmd));

    let oplog = priConn.getDB("local").oplog.rs;
    let insertOplogEntries = oplog.find({ns: "test.user", op: "i"}).itcount();

    let testDBPri = priConn.getDB("test");
    assert.eq(2, testDBPri.user.find().itcount());

    let createdCollectionInTxn = handleSessionsCollection(mainConn, priConn, configConn);

    let retryResult = assert.commandWorked(testDBMain.runCommand(cmd));
    assert.eq(result.ok, retryResult.ok);
    assert.eq(result.n, retryResult.n);
    assert.eq(result.writeErrors, retryResult.writeErrors);
    assert.eq(result.writeConcernErrors, retryResult.writeConcernErrors);

    assert.eq(2, testDBPri.user.find().itcount());
    assert.eq(insertOplogEntries, oplog.find({ns: "test.user", op: "i"}).itcount());

    let newStatus = priConn.adminCommand({serverStatus: 1});
    verifyServerStatusFields(newStatus);
    verifyServerStatusChanges(
        initialStatus.transactions,
        newStatus.transactions,
        1 /* newCommands */,
        2 /* newStatements */,
        createdCollectionInTxn ? 2 : 1 /* newCollectionWrites */,
    );

    ////////////////////////////////////////////////////////////////////////
    // Test update command

    initialStatus = priConn.adminCommand({serverStatus: 1});
    verifyServerStatusFields(initialStatus);

    cmd = {
        update: "user",
        updates: [
            {q: {_id: 10}, u: {$inc: {x: 1}}}, // in place
            {q: {_id: 20}, u: {$inc: {y: 1}}, upsert: true},
            {q: {_id: 30}, u: {z: 1}}, // replacement
        ],
        ordered: false,
        lsid: {id: lsid},
        txnNumber: NumberLong(35),
    };

    result = assert.commandWorked(testDBMain.runCommand(cmd));

    let updateOplogEntries = oplog.find({ns: "test.user", op: "u"}).itcount();

    // Upserts are stored as inserts in the oplog, so check inserts too.
    insertOplogEntries = oplog.find({ns: "test.user", op: "i"}).itcount();

    assert.eq(3, testDBPri.user.find().itcount());

    createdCollectionInTxn = handleSessionsCollection(mainConn, priConn, configConn);

    retryResult = assert.commandWorked(testDBMain.runCommand(cmd));
    assert.eq(result.ok, retryResult.ok);
    assert.eq(result.n, retryResult.n);
    assert.eq(result.nModified, retryResult.nModified);
    assert.eq(result.upserted, retryResult.upserted);
    assert.eq(result.writeErrors, retryResult.writeErrors);
    assert.eq(result.writeConcernErrors, retryResult.writeConcernErrors);

    assert.eq(3, testDBPri.user.find().itcount());

    assert.eq({_id: 10, x: 1}, testDBPri.user.findOne({_id: 10}));
    assert.eq({_id: 20, y: 1}, testDBPri.user.findOne({_id: 20}));
    assert.eq({_id: 30, z: 1}, testDBPri.user.findOne({_id: 30}));

    assert.eq(updateOplogEntries, oplog.find({ns: "test.user", op: "u"}).itcount());
    assert.eq(insertOplogEntries, oplog.find({ns: "test.user", op: "i"}).itcount());

    newStatus = priConn.adminCommand({serverStatus: 1});
    verifyServerStatusFields(newStatus);
    verifyServerStatusChanges(
        initialStatus.transactions,
        newStatus.transactions,
        1 /* newCommands */,
        3 /* newStatements */,
        createdCollectionInTxn ? 4 : 3 /* newCollectionWrites */,
    );

    ////////////////////////////////////////////////////////////////////////
    // Test delete command

    initialStatus = priConn.adminCommand({serverStatus: 1});
    verifyServerStatusFields(initialStatus);

    assert.commandWorked(testDBMain.user.insert({_id: 40, x: 1}));
    assert.commandWorked(testDBMain.user.insert({_id: 50, y: 1}));

    assert.eq(2, testDBPri.user.find({x: 1}).itcount());
    assert.eq(2, testDBPri.user.find({y: 1}).itcount());

    cmd = {
        delete: "user",
        deletes: [
            {q: {x: 1}, limit: 1},
            {q: {y: 1}, limit: 1},
        ],
        ordered: false,
        lsid: {id: lsid},
        txnNumber: NumberLong(36),
    };

    result = assert.commandWorked(mainConn.getDB("test").runCommand(cmd));

    let deleteOplogEntries = oplog.find({ns: "test.user", op: "d"}).itcount();

    assert.eq(1, testDBPri.user.find({x: 1}).itcount());
    assert.eq(1, testDBPri.user.find({y: 1}).itcount());

    createdCollectionInTxn = handleSessionsCollection(mainConn, priConn, configConn);

    retryResult = assert.commandWorked(testDBMain.runCommand(cmd));
    assert.eq(result.ok, retryResult.ok);
    assert.eq(result.n, retryResult.n);
    assert.eq(result.writeErrors, retryResult.writeErrors);
    assert.eq(result.writeConcernErrors, retryResult.writeConcernErrors);

    assert.eq(1, testDBPri.user.find({x: 1}).itcount());
    assert.eq(1, testDBPri.user.find({y: 1}).itcount());

    assert.eq(deleteOplogEntries, oplog.find({ns: "test.user", op: "d"}).itcount());

    newStatus = priConn.adminCommand({serverStatus: 1});
    verifyServerStatusFields(newStatus);
    verifyServerStatusChanges(
        initialStatus.transactions,
        newStatus.transactions,
        1 /* newCommands */,
        2 /* newStatements */,
        createdCollectionInTxn ? 3 : 2 /* newCollectionWrites */,
    );

    ////////////////////////////////////////////////////////////////////////
    // Test findAndModify command (upsert)

    initialStatus = priConn.adminCommand({serverStatus: 1});
    verifyServerStatusFields(initialStatus);

    cmd = {
        findAndModify: "user",
        query: {_id: 60},
        update: {$inc: {x: 1}},
        new: true,
        upsert: true,
        lsid: {id: lsid},
        txnNumber: NumberLong(37),
    };

    result = assert.commandWorked(mainConn.getDB("test").runCommand(cmd));
    insertOplogEntries = oplog.find({ns: "test.user", op: "i"}).itcount();
    updateOplogEntries = oplog.find({ns: "test.user", op: "u"}).itcount();
    assert.eq({_id: 60, x: 1}, testDBPri.user.findOne({_id: 60}));

    createdCollectionInTxn = handleSessionsCollection(mainConn, priConn, configConn);

    retryResult = assert.commandWorked(testDBMain.runCommand(cmd));

    assert.eq({_id: 60, x: 1}, testDBPri.user.findOne({_id: 60}));
    assert.eq(insertOplogEntries, oplog.find({ns: "test.user", op: "i"}).itcount());
    assert.eq(updateOplogEntries, oplog.find({ns: "test.user", op: "u"}).itcount());

    checkFindAndModifyResult(result, retryResult);

    newStatus = priConn.adminCommand({serverStatus: 1});
    verifyServerStatusFields(newStatus);
    verifyServerStatusChanges(
        initialStatus.transactions,
        newStatus.transactions,
        1 /* newCommands */,
        1 /* newStatements */,
        createdCollectionInTxn ? 2 : 1 /* newCollectionWrites */,
    );

    ////////////////////////////////////////////////////////////////////////
    // Test findAndModify command (update, return pre-image)

    initialStatus = priConn.adminCommand({serverStatus: 1});
    verifyServerStatusFields(initialStatus);

    cmd = {
        findAndModify: "user",
        query: {_id: 60},
        update: {$inc: {x: 1}},
        new: false,
        upsert: false,
        lsid: {id: lsid},
        txnNumber: NumberLong(38),
    };

    result = assert.commandWorked(mainConn.getDB("test").runCommand(cmd));
    let oplogEntries = oplog.find({ns: "test.user", op: "u"}).itcount();
    assert.eq({_id: 60, x: 2}, testDBPri.user.findOne({_id: 60}));

    createdCollectionInTxn = handleSessionsCollection(mainConn, priConn, configConn);

    retryResult = assert.commandWorked(testDBMain.runCommand(cmd));

    assert.eq({_id: 60, x: 2}, testDBPri.user.findOne({_id: 60}));
    assert.eq(oplogEntries, oplog.find({ns: "test.user", op: "u"}).itcount());

    checkFindAndModifyResult(result, retryResult);

    newStatus = priConn.adminCommand({serverStatus: 1});
    verifyServerStatusFields(newStatus);
    verifyServerStatusChanges(
        initialStatus.transactions,
        newStatus.transactions,
        1 /* newCommands */,
        1 /* newStatements */,
        createdCollectionInTxn ? 2 : 1 /* newCollectionWrites */,
    );

    ////////////////////////////////////////////////////////////////////////
    // Test findAndModify command (update, return post-image)

    initialStatus = priConn.adminCommand({serverStatus: 1});
    verifyServerStatusFields(initialStatus);

    cmd = {
        findAndModify: "user",
        query: {_id: 60},
        update: {$inc: {x: 1}},
        new: true,
        upsert: false,
        lsid: {id: lsid},
        txnNumber: NumberLong(39),
    };

    result = assert.commandWorked(mainConn.getDB("test").runCommand(cmd));
    oplogEntries = oplog.find({ns: "test.user", op: "u"}).itcount();
    assert.eq({_id: 60, x: 3}, testDBPri.user.findOne({_id: 60}));

    createdCollectionInTxn = handleSessionsCollection(mainConn, priConn, configConn);

    retryResult = assert.commandWorked(testDBMain.runCommand(cmd));

    assert.eq({_id: 60, x: 3}, testDBPri.user.findOne({_id: 60}));
    assert.eq(oplogEntries, oplog.find({ns: "test.user", op: "u"}).itcount());

    checkFindAndModifyResult(result, retryResult);

    newStatus = priConn.adminCommand({serverStatus: 1});
    verifyServerStatusFields(newStatus);
    verifyServerStatusChanges(
        initialStatus.transactions,
        newStatus.transactions,
        1 /* newCommands */,
        1 /* newStatements */,
        createdCollectionInTxn ? 2 : 1 /* newCollectionWrites */,
    );

    ////////////////////////////////////////////////////////////////////////
    // Test findAndModify command (remove, return pre-image)

    initialStatus = priConn.adminCommand({serverStatus: 1});
    verifyServerStatusFields(initialStatus);

    assert.commandWorked(testDBMain.user.insert({_id: 70, f: 1}));
    assert.commandWorked(testDBMain.user.insert({_id: 80, f: 1}));

    cmd = {
        findAndModify: "user",
        query: {f: 1},
        remove: true,
        lsid: {id: lsid},
        txnNumber: NumberLong(40),
    };

    result = assert.commandWorked(mainConn.getDB("test").runCommand(cmd));
    oplogEntries = oplog.find({ns: "test.user", op: "d"}).itcount();
    let docCount = testDBPri.user.find().itcount();

    createdCollectionInTxn = handleSessionsCollection(mainConn, priConn, configConn);

    retryResult = assert.commandWorked(testDBMain.runCommand(cmd));

    assert.eq(oplogEntries, oplog.find({ns: "test.user", op: "d"}).itcount());
    assert.eq(docCount, testDBPri.user.find().itcount());

    checkFindAndModifyResult(result, retryResult);

    newStatus = priConn.adminCommand({serverStatus: 1});
    verifyServerStatusFields(newStatus);
    verifyServerStatusChanges(
        initialStatus.transactions,
        newStatus.transactions,
        1 /* newCommands */,
        1 /* newStatements */,
        createdCollectionInTxn ? 2 : 1 /* newCollectionWrites */,
    );
}

function runFailpointTests(mainConn, priConn) {
    // Test the 'onPrimaryTransactionalWrite' failpoint
    let lsid = UUID();
    let testDb = mainConn.getDB("TestDB");
    assert.commandWorked(testDb.createCollection("user"));

    // Test connection close (default behaviour). The connection will get closed, but the
    // inserts must succeed
    assert.commandWorked(priConn.adminCommand({configureFailPoint: "onPrimaryTransactionalWrite", mode: "alwaysOn"}));

    try {
        // Set skipRetryOnNetworkError so the shell doesn't automatically retry, since the
        // command has a txnNumber.
        TestData.skipRetryOnNetworkError = true;
        let res = assert.commandWorked(
            testDb.runCommand({
                insert: "user",
                documents: [{x: 0}, {x: 1}],
                ordered: true,
                lsid: {id: lsid},
                txnNumber: NumberLong(1),
            }),
        );
        // Mongos will automatically retry on retryable errors if the request has a txnNumber,
        // and the retry path for already completed writes does not trigger the failpoint, so
        // the command will succeed when run through mongos.
        assert.eq(2, res.n);
        assert.eq(false, res.hasOwnProperty("writeErrors"));
    } catch (e) {
        let exceptionMsg = e.toString();
        assert(isNetworkError(e), "Incorrect exception thrown: " + exceptionMsg);
    } finally {
        TestData.skipRetryOnNetworkError = false;
    }

    let collCount = 0;
    assert.soon(
        () => {
            collCount = testDb.user.find({}).itcount();
            return collCount == 2;
        },
        "testDb.user returned " + collCount + " entries",
    );

    // Test exception throw. One update must succeed and the other must fail.
    assert.commandWorked(
        priConn.adminCommand({
            configureFailPoint: "onPrimaryTransactionalWrite",
            mode: {skip: 1},
            data: {closeConnection: false, failBeforeCommitExceptionCode: ErrorCodes.InternalError},
        }),
    );

    let cmd = {
        update: "user",
        updates: [
            {q: {x: 0}, u: {$inc: {y: 1}}},
            {q: {x: 1}, u: {$inc: {y: 1}}},
        ],
        ordered: true,
        lsid: {id: lsid},
        txnNumber: NumberLong(2),
    };

    let writeResult = testDb.runCommand(cmd);

    assert.eq(1, writeResult.nModified);
    assert.eq(1, writeResult.writeErrors.length);
    assert.eq(1, writeResult.writeErrors[0].index);
    assert.eq(ErrorCodes.InternalError, writeResult.writeErrors[0].code);

    assert.commandWorked(priConn.adminCommand({configureFailPoint: "onPrimaryTransactionalWrite", mode: "off"}));

    writeResult = testDb.runCommand(cmd);
    assert.eq(2, writeResult.nModified);

    let collContents = testDb.user.find({}).sort({x: 1}).toArray();
    assert.eq(2, collContents.length);
    assert.eq(0, collContents[0].x);
    assert.eq(1, collContents[0].y);
    assert.eq(1, collContents[1].x);
    assert.eq(1, collContents[1].y);
}

function runRetryableWriteErrorTest(mainConn) {
    // Test TransactionTooOld error message on retryable writes
    const lsid = UUID();
    const testDb = mainConn.getDB("TestDB");

    assert.commandWorked(
        testDb.runCommand({
            insert: "user",
            documents: [{x: 1}],
            ordered: true,
            lsid: {id: lsid},
            txnNumber: NumberLong(2),
        }),
    );
    const writeResult = testDb.runCommand({
        update: "user",
        updates: [{q: {x: 1}, u: {$inc: {x: 1}}}],
        ordered: true,
        lsid: {id: lsid},
        txnNumber: NumberLong(1),
    });
    assert.commandFailedWithCode(writeResult, ErrorCodes.TransactionTooOld);
    assert(writeResult.errmsg.includes("Retryable write with txnNumber 1 is prohibited"), writeResult);
}

function runMultiTests(mainConn) {
    // Test the behavior of retryable writes with multi=true / limit=0
    let lsid = {id: UUID()};
    let testDb = mainConn.getDB("test_multi");

    // Only the update statements with multi=true in a batch fail.
    let cmd = {
        update: "user",
        updates: [
            {q: {x: 1}, u: {y: 1}},
            {q: {x: 2}, u: {z: 1}, multi: true},
        ],
        ordered: true,
        lsid: lsid,
        txnNumber: NumberLong(1),
    };
    let res = assert.commandWorkedIgnoringWriteErrors(testDb.runCommand(cmd));
    assert.eq(1, res.writeErrors.length, "expected only one write error, received: " + tojson(res.writeErrors));
    assert.eq(
        1,
        res.writeErrors[0].index,
        "expected the update at index 1 to fail, not the update at index: " + res.writeErrors[0].index,
    );
    assert.eq(
        ErrorCodes.InvalidOptions,
        res.writeErrors[0].code,
        "expected to fail with code " + ErrorCodes.InvalidOptions + ", received: " + res.writeErrors[0].code,
    );

    // Only the delete statements with limit=0 in a batch fail.
    cmd = {
        delete: "user",
        deletes: [
            {q: {x: 1}, limit: 1},
            {q: {y: 1}, limit: 0},
        ],
        ordered: false,
        lsid: lsid,
        txnNumber: NumberLong(1),
    };
    res = assert.commandWorkedIgnoringWriteErrors(testDb.runCommand(cmd));
    assert.eq(1, res.writeErrors.length, "expected only one write error, received: " + tojson(res.writeErrors));
    assert.eq(
        1,
        res.writeErrors[0].index,
        "expected the delete at index 1 to fail, not the delete at index: " + res.writeErrors[0].index,
    );
    assert.eq(
        ErrorCodes.InvalidOptions,
        res.writeErrors[0].code,
        "expected to fail with code " + ErrorCodes.InvalidOptions + ", received: " + res.writeErrors[0].code,
    );
}

function runInvalidTests(mainConn) {
    let lsid = {id: UUID()};
    let localDB = mainConn.getDB("local");

    let cmd = {
        insert: "user",
        documents: [{_id: 10}, {_id: 30}],
        ordered: false,
        lsid: lsid,
        txnNumber: NumberLong(10),
    };

    let res = assert.commandWorkedIgnoringWriteErrors(localDB.runCommand(cmd));
    assert.eq(2, res.writeErrors.length);

    localDB.user.insert({_id: 10, x: 1});
    localDB.user.insert({_id: 30, z: 2});

    cmd = {
        update: "user",
        updates: [
            {q: {_id: 10}, u: {$inc: {x: 1}}}, // in place
            {q: {_id: 20}, u: {$inc: {y: 1}}, upsert: true},
            {q: {_id: 30}, u: {z: 1}}, // replacement
        ],
        ordered: false,
        lsid: lsid,
        txnNumber: NumberLong(11),
    };

    res = assert.commandWorkedIgnoringWriteErrors(localDB.runCommand(cmd));
    assert.eq(3, res.writeErrors.length);

    cmd = {
        delete: "user",
        deletes: [
            {q: {x: 1}, limit: 1},
            {q: {z: 2}, limit: 1},
        ],
        ordered: false,
        lsid: lsid,
        txnNumber: NumberLong(12),
    };

    res = assert.commandWorkedIgnoringWriteErrors(localDB.runCommand(cmd));
    assert.eq(2, res.writeErrors.length);

    cmd = {
        findAndModify: "user",
        query: {_id: 60},
        update: {$inc: {x: 1}},
        new: true,
        upsert: true,
        lsid: {id: lsid},
        txnNumber: NumberLong(37),
    };

    assert.commandFailed(localDB.runCommand(cmd));
}

// Tests for replica set
let replTest = new ReplSetTest({nodes: 2});
replTest.startSet({verbose: 5});
replTest.initiate();

let priConn = replTest.getPrimary();

runTests(priConn, priConn, priConn);
runFailpointTests(priConn, priConn);
runRetryableWriteErrorTest(priConn);
runMultiTests(priConn);
runInvalidTests(priConn);

replTest.stopSet();

// Tests for sharded cluster
let st = new ShardingTest({shards: {rs0: {nodes: 1, verbose: 5}}});

runTests(st.s0, st.rs0.getPrimary(), st.configRS.getPrimary());
runFailpointTests(st.s0, st.rs0.getPrimary());
runMultiTests(st.s0);

st.stop();
