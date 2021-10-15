/**
 * Tests that mongod and mongos
 * - Do not throw an error if a client specifies readConcern for an insert, update, delete, and
 *   findAndModify command running as the first command in a transaction.
 * - Throw InvalidOptions if a client specifies readConcern in all other cases.
 * @tags: [requires_fcv_51, uses_transactions, uses_multi_shard_transaction]
 */
(function() {
'use strict';

const st = new ShardingTest({
    mongos: 1,
    shards: 1,
});
const shard0Primary = st.rs0.getPrimary();

const kDbName = "testDb";
assert.commandWorked(st.s.adminCommand({enableSharding: kDbName}));

function testWriteCommandOutsideSessionAndTransaction(
    conn, cmdObj, readConcern, shouldBypassCheck) {
    const cmdObjWithReadConcern = Object.assign({}, cmdObj, {
        readConcern: readConcern,
    });
    const res = conn.getDB(kDbName).runCommand(cmdObjWithReadConcern);
    if (shouldBypassCheck) {
        assert.commandWorked(res);
    } else {
        assert.commandFailedWithCode(res, ErrorCodes.InvalidOptions);
    }
}

function testWriteCommandOutsideTransaction(conn, cmdObj, readConcern, shouldBypassCheck) {
    const lsid = {id: UUID()};
    const cmdObjWithReadConcern = Object.assign({}, cmdObj, {readConcern: readConcern, lsid: lsid});
    const res = conn.getDB(kDbName).runCommand(cmdObjWithReadConcern);
    if (shouldBypassCheck) {
        assert.commandWorked(res);
    } else {
        assert.commandFailedWithCode(res, ErrorCodes.InvalidOptions);
    }
}

function testWriteCommandInsideTransactionFirstCommand(conn, cmdObj, readConcern) {
    const lsid = {id: UUID()};
    const txnNumber = NumberLong(1);
    const cmdObjWithReadConcern = Object.assign({}, cmdObj, {
        readConcern: readConcern,
        lsid: lsid,
        txnNumber: txnNumber,
        startTransaction: true,
        autocommit: false
    });

    assert.commandWorked(conn.getDB(kDbName).runCommand(cmdObjWithReadConcern));
    assert.commandWorked(conn.getDB(kDbName).adminCommand({
        commitTransaction: 1,
        lsid: lsid,
        txnNumber: txnNumber,
        autocommit: false,
        writeConcern: {w: "majority"}
    }));
}

function testWriteCommandInsideTransactionNotFirstCommand(conn, cmdObj, readConcern, kCollName) {
    const lsid = {id: UUID()};
    const txnNumber = NumberLong(1);

    assert.commandWorked(conn.getDB(kDbName).runCommand({
        findAndModify: kCollName,
        query: {x: -10},
        update: {$set: {z: -10}},
        lsid: lsid,
        txnNumber: txnNumber,
        startTransaction: true,
        autocommit: false
    }));
    const cmdObjWithReadConcern = Object.assign(
        {},
        cmdObj,
        {readConcern: readConcern, lsid: lsid, txnNumber: txnNumber, autocommit: false});
    const res = conn.getDB(kDbName).runCommand(cmdObjWithReadConcern);
    assert.commandFailedWithCode(res, ErrorCodes.InvalidOptions);
    assert.commandWorked(conn.getDB(kDbName).adminCommand({
        abortTransaction: 1,
        lsid: lsid,
        txnNumber: txnNumber,
        autocommit: false,
        writeConcern: {w: "majority"}
    }));
}

function runTest(conn, cmdObj, mongosConn, kCollName) {
    const defaultReadConcerns = [
        {},
        {"level": "local"},
        {"level": "majority"},
        {"level": "available"},
    ];

    defaultReadConcerns.forEach(defaultReadConcern => {
        if ("level" in defaultReadConcern) {
            assert.commandWorked(mongosConn.adminCommand({
                setDefaultRWConcern: 1,
                defaultReadConcern: defaultReadConcern,
            }));
            jsTest.log("Testing defaultReadConcern " + tojson(defaultReadConcern));
        } else {
            jsTest.log("Testing without specifying defaultReadConcern");
        }

        const testCases = [
            {supportedInTransaction: true, readConcern: {level: "snapshot"}},
            {supportedInTransaction: true, readConcern: {level: "majority"}},
            {supportedInTransaction: false, readConcern: {level: "linearizable"}},
            {supportedInTransaction: false, readConcern: {level: "available"}},
            // "local" is the default readConcern so it is exempt from the check.
            {supportedInTransaction: true, readConcern: {level: "local"}, shouldBypassCheck: true}
        ];
        testCases.forEach(testCase => {
            jsTest.log("Testing readConcern " + tojson(testCase.readConcern));
            testWriteCommandOutsideSessionAndTransaction(
                conn, cmdObj, testCase.readConcern, testCase.shouldBypassCheck);
            testWriteCommandOutsideTransaction(
                conn, cmdObj, testCase.readConcern, testCase.shouldBypassCheck);
            if (testCase.supportedInTransaction) {
                testWriteCommandInsideTransactionFirstCommand(conn, cmdObj, testCase.readConcern);
                testWriteCommandInsideTransactionNotFirstCommand(
                    conn, cmdObj, testCase.readConcern, kCollName);
            }
        });
    });
}

function runTests(conn, mongosConn, kCollName) {
    const kNs = kDbName + "." + kCollName;

    // Do an insert to force a refresh so the first transaction doesn't fail due to StaleConfig.
    assert.commandWorked(st.s.getCollection(kNs).insert({x: 0}));

    jsTest.log("Running insert");
    const insertCmdObj = {
        insert: kCollName,
        documents: [{x: -10}],
    };
    runTest(conn, insertCmdObj, mongosConn, kCollName);

    jsTest.log("Running update");
    const updateCmdObj = {
        update: kCollName,
        updates: [{q: {x: -10}, u: {$set: {y: -10}}}],
    };
    runTest(conn, updateCmdObj, mongosConn, kCollName);

    jsTest.log("Running delete");
    const deleteCmdObj = {
        delete: kCollName,
        deletes: [{q: {x: -10}, limit: 1}],
    };
    runTest(conn, deleteCmdObj, mongosConn, kCollName);

    jsTest.log("Running findAndModify");
    const findAndModifyCmdObj = {
        findAndModify: kCollName,
        query: {x: -10},
        update: {$set: {z: -10}},
    };
    runTest(conn, findAndModifyCmdObj, mongosConn, kCollName);
}

jsTest.log("Running tests against mongod");
const kCollName0 = "testColl0";
runTests(shard0Primary, st.s, kCollName0);

jsTest.log("Running tests against mongos");
const kCollName1 = "testColl1";
runTests(st.s, st.s, kCollName1);

st.stop();
})();
