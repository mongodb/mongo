/*
 * Basic tests confirming functionality of the interalTransactionsTestCommand.
 *
 * @tags: [requires_fcv_60, uses_transactions]
 */
(function() {
'use strict';

// This test intentionally runs commands without a logical session id, which is not compatible
// with implicit sessions.
TestData.disableImplicitSessions = true;

const kDbName = "testDb";
const kCollName = "testColl";
const kNs = kDbName + "." + kCollName;

const st = new ShardingTest({shards: 1});
const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let db = primary.getDB(kDbName);
let rstColl = db.getCollection(kCollName);

let stColl = st.s.getCollection(kNs);

function verifyCompletedInsertCommandResult(commandInfos, response, collection) {
    let i = 0;
    commandInfos.forEach(commandInfo => {
        // Verifies command response.
        assert.eq(1, response.responses[i].ok);
        assert.eq(commandInfo.command.documents.length, response.responses[i].n);
        commandInfo.command.documents.forEach(document => {
            // Verifies documents were successfully inserted.
            assert.eq(document, collection.findOne(document));
        });
        ++i;
    });
}

function runTxn(connection, commandInfos, collection) {
    const res = assert.commandWorked(
        connection.adminCommand({testInternalTransactions: 1, commandInfos: commandInfos}));
    verifyCompletedInsertCommandResult(commandInfos, res, collection);
}

function runRetryableWrite(connection, commandInfos, lsid) {
    const txnNumber = NumberLong(0);
    return assert.commandWorked(connection.adminCommand({
        testInternalTransactions: 1,
        commandInfos: commandInfos,
        lsid: {id: lsid},
        txnNumber: txnNumber
    }));
}

function runRetryableInsert(connection, commandInfos, lsid, collection) {
    const originalRes = runRetryableWrite(connection, commandInfos, lsid);
    verifyCompletedInsertCommandResult(commandInfos, originalRes, collection);

    // This retryable write is expected to return a response of length 1 as the command list should
    // stop executing after the first insert.
    const retryRes = runRetryableWrite(connection, commandInfos, lsid);
    assert.eq(1, retryRes.responses.length);
    assert.eq(1, retryRes.ok);
    assert.eq(0, retryRes.responses[0].retriedStmtIds[0]);
}

function runRetryableFindAndModify(connection, commandInfos, lsid, collection) {
    const originalRes = runRetryableWrite(connection, commandInfos, lsid);
    assert.eq(commandInfos.length, originalRes.responses.length);

    // Verify command response.
    assert.eq(1, originalRes.responses[0].lastErrorObject.n);
    assert.eq(commandInfos[0].command.query._id, originalRes.responses[0].lastErrorObject.upserted);

    // Verify document from commandInfos[0] was upserted into database.
    let document = commandInfos[0].command.query;
    assert.eq(document, collection.findOne(document));

    // Verify document from commandInfos[1] was inserted into database.
    assert.eq(1, originalRes.responses[1].n);
    document = commandInfos[1].command.documents[0];
    assert.eq(document, collection.findOne(document));

    // Retry transaction. This retryable write is expected to return a response of length 1 as the
    // command list should stop executing after the first findAndModify command.
    const retryRes = runRetryableWrite(connection, commandInfos, lsid);
    assert.eq(1, retryRes.responses.length);
    assert.eq(1, retryRes.ok);
    assert.eq(0, retryRes.responses[0].retriedStmtId);
}

// Insert initial data.
assert.commandWorked(stColl.insert([{_id: 0}]));
assert.commandWorked(rstColl.insert([{_id: 0}]));

// Set of commandInfos that will be used in tests below.
const commandInfosSingleInsert = [{
    dbName: kDbName,
    command: {
        insert: kCollName,
        documents: [{_id: 1}],
    }
}];

const commandInfosBatchInsert = [
    {
        dbName: kDbName,
        command: {
            insert: kCollName,
            documents: [{_id: 2}],
        }
    },
    {
        dbName: kDbName,
        command: {
            insert: kCollName,
            documents: [{_id: 3}, {_id: 4}],
        }
    }
];

const commandInfosRetryableBatchInsert = [
    {
        dbName: kDbName,
        command: {
            insert: kCollName,
            documents: [{_id: 5}],
            stmtId: NumberInt(0),
        }
    },
    {
        dbName: kDbName,
        command: {
            insert: kCollName,
            documents: [{_id: 6}, {_id: 7}],
            stmtId: NumberInt(1),
        }
    }
];

function commandInfosRetryableFindAndModify(collection) {
    return [
        {
            dbName: kDbName,
            command: {
                findandmodify: collection.getName(),
                query: {_id: 8},
                update: {},
                upsert: true,
                stmtId: NumberInt(0),
            }
        },
        {
            dbName: kDbName,
            command: {
                insert: collection.getName(),
                documents: [{_id: 9}],
                stmtId: NumberInt(1),
            }
        }
    ];
}

jsTest.log(
    "Insert documents without a session into a sharded cluster, using internal transactions test command.");
runTxn(st.s, commandInfosSingleInsert, stColl);
runTxn(st.s, commandInfosBatchInsert, stColl);

jsTest.log(
    "Insert documents without a session into a replica set, using internal transactions test command.");
runTxn(primary, commandInfosSingleInsert, rstColl);
runTxn(primary, commandInfosBatchInsert, rstColl);

jsTest.log("Testing retryable write targeting a mongos.");
runRetryableInsert(st.s, commandInfosRetryableBatchInsert, UUID(), stColl);
runRetryableFindAndModify(st.s, commandInfosRetryableFindAndModify(stColl), UUID(), stColl);

jsTest.log("Testing retryable write targeting a mongod.");
runRetryableInsert(primary, commandInfosRetryableBatchInsert, UUID(), rstColl);
runRetryableFindAndModify(primary, commandInfosRetryableFindAndModify(rstColl), UUID(), rstColl);

rst.stopSet();
st.stop();
})();
