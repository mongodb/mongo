/*
 * Test that retryable writes executed using or without using internal transactions execute exactly
 * once regardless of how they are batched on retries, and that the responses from mongods include a
 * "retriedStmtIds" field containing the statement ids for retried statements.
 *
 * @tags: [requires_fcv_52, featureFlagInternalTransactions]
 */
(function() {
"use strict";

load("jstests/sharding/libs/sharded_transactions_helpers.js");

const st = new ShardingTest({shards: 1});

const kDbName = "testDb";
const kCollName = "testColl";
const shard0TestDB = st.rs0.getPrimary().getDB(kDbName);
const shard0TestColl = shard0TestDB.getCollection(kCollName);

const stmtId1 = NumberInt(1);
const stmtId2 = NumberInt(2);

/*
 * Returns a new command object created from 'cmdObj' and the session/transaction fields defined in
 * 'sessionOpts'.
 */
function makeCmdObjWithTxnFields(cmdObj, sessionOpts) {
    const cmdObjWithTxnFields = Object.assign({}, cmdObj);
    cmdObjWithTxnFields.lsid = sessionOpts.lsid;
    cmdObjWithTxnFields.txnNumber = sessionOpts.txnNumber;
    if (sessionOpts.isTransaction) {
        cmdObjWithTxnFields.autocommit = false;
    }
    return cmdObjWithTxnFields;
}

/*
 * Runs all the commands in 'cmdObjs' in a retryable write or transaction as defined in
 * 'sessionOpts', and returns the responses to those commands in the given order.
 */
function runCommandsWithSessionOpts(cmdObjs, sessionOpts) {
    let cmdResponses = [];

    cmdObjs.forEach((cmdObj, index) => {
        const cmdObjWithTxnFields = makeCmdObjWithTxnFields(cmdObj, sessionOpts);
        if (sessionOpts.isTransaction && index == 0) {
            cmdObjWithTxnFields.startTransaction = true;
        }
        cmdResponses.push(assert.commandWorked(shard0TestDB.runCommand(cmdObjWithTxnFields)));
    });
    if (sessionOpts.isTransaction) {
        assert.commandWorked(shard0TestDB.adminCommand(
            makeCommitTransactionCmdObj(sessionOpts.lsid, sessionOpts.txnNumber)));
    }

    return cmdResponses;
}

// Test that write statements executed in the same command do not re-executed when they are
// present in separate commands, and the right stmtId is returned in the "retriedStmtIds" field in
// each retry response.

function runInsertTestRetryStatementsSeparately(makeSessionOptionsFunc) {
    const {initialSessionOpts, retrySessionOpts} = makeSessionOptionsFunc();

    jsTest.log(`Test executing two insert statements in a single command in ${
        tojson(initialSessionOpts)} then retrying them in two separate commands in ${
        tojson(retrySessionOpts)}`);

    const [initialRes] = runCommandsWithSessionOpts(
        [{insert: kCollName, documents: [{_id: 1}, {_id: 2}], stmtIds: [stmtId1, stmtId2]}],
        initialSessionOpts);
    const [retryRes1, retryRes2] = runCommandsWithSessionOpts(
        [
            {insert: kCollName, documents: [{_id: 1}], stmtIds: [stmtId1]},
            {insert: kCollName, documents: [{_id: 2}], stmtIds: [stmtId2]}
        ],
        retrySessionOpts);

    assert.eq(initialRes.n, 2);
    assert(!initialRes.hasOwnProperty("retriedStmtIds"));
    assert.eq(retryRes1.n, 1);
    assert.eq(retryRes1.retriedStmtIds, [stmtId1]);
    assert.eq(retryRes2.n, 1);
    assert.eq(retryRes2.retriedStmtIds, [stmtId2]);
    assert.eq(shard0TestColl.find({_id: 1}).itcount(), 1);
    assert.eq(shard0TestColl.find({_id: 2}).itcount(), 1);

    assert.commandWorked(shard0TestColl.remove({}));
}

function runUpdateTestRetryStatementsSeparately(makeSessionOptionsFunc) {
    const {initialSessionOpts, retrySessionOpts} = makeSessionOptionsFunc();

    jsTest.log(`Test executing two update statements in a single command in ${
        tojson(initialSessionOpts)} then retrying them in two separate commands in ${
        tojson(retrySessionOpts)}`);

    assert.commandWorked(shard0TestColl.insert([{_id: 1, x: 0}, {_id: 2, x: 0}]));

    const [initialRes] = runCommandsWithSessionOpts(
        [{
            update: kCollName,
            updates: [{q: {_id: 1}, u: {$inc: {x: 1}}}, {q: {_id: 2}, u: {$inc: {x: 1}}}],
            stmtIds: [stmtId1, stmtId2]
        }],
        initialSessionOpts);
    const [retryRes1, retryRes2] = runCommandsWithSessionOpts(
        [
            {update: kCollName, updates: [{q: {_id: 1}, u: {$inc: {x: 1}}}], stmtIds: [stmtId1]},
            {update: kCollName, updates: [{q: {_id: 2}, u: {$inc: {x: 1}}}], stmtIds: [stmtId2]}
        ],
        retrySessionOpts);

    assert.eq(initialRes.nModified, 2);
    assert(!initialRes.hasOwnProperty("retriedStmtIds"));
    assert.eq(retryRes1.nModified, 1);
    assert.eq(retryRes1.retriedStmtIds, [stmtId1]);
    assert.eq(retryRes2.nModified, 1);
    assert.eq(retryRes2.retriedStmtIds, [stmtId2]);
    assert.eq(shard0TestColl.find({_id: 1, x: 1}).itcount(), 1);
    assert.eq(shard0TestColl.find({_id: 2, x: 1}).itcount(), 1);

    assert.commandWorked(shard0TestColl.remove({}));
}

function runDeleteTestRetryStatementsSeparately(makeSessionOptionsFunc) {
    const {initialSessionOpts, retrySessionOpts} = makeSessionOptionsFunc();

    jsTest.log(`Test executing two delete statements in a single command in ${
        tojson(initialSessionOpts)} then retrying them in two separate commands in ${
        tojson(retrySessionOpts)}`);

    assert.commandWorked(shard0TestColl.insert([{_id: 1}, {_id: 2}]));

    const [initialRes] =
        runCommandsWithSessionOpts([{
                                       delete: kCollName,
                                       deletes: [{q: {_id: 1}, limit: 1}, {q: {_id: 2}, limit: 1}],
                                       stmtIds: [stmtId1, stmtId2]
                                   }],
                                   initialSessionOpts);
    const [retryRes1, retryRes2] = runCommandsWithSessionOpts(
        [
            {delete: kCollName, deletes: [{q: {_id: 1}, limit: 1}], stmtIds: [stmtId1]},
            {delete: kCollName, deletes: [{q: {_id: 2}, limit: 1}], stmtIds: [stmtId2]}
        ],
        retrySessionOpts);

    assert.eq(initialRes.n, 2);
    assert(!initialRes.hasOwnProperty("retriedStmtIds"));
    assert.eq(retryRes1.n, 1);
    assert.eq(retryRes1.retriedStmtIds, [stmtId1]);
    assert.eq(retryRes2.n, 1);
    assert.eq(retryRes2.retriedStmtIds, [stmtId2]);
    assert.eq(shard0TestColl.find({_id: 1}).itcount(), 0);
    assert.eq(shard0TestColl.find({_id: 2}).itcount(), 0);

    assert.commandWorked(shard0TestColl.remove({}));
}

// Test that an executed write statement does not re-execute when it is present in a command
// containing un-executed write statements, and that its stmtId is returned in the "retriedStmtId"
// field in the response.

function runInsertTestRetryWithAdditionalStatement(makeSessionOptionsFunc) {
    const {initialSessionOpts, retrySessionOpts} = makeSessionOptionsFunc();

    jsTest.log(`Test executing an insert statement in a command in ${
        tojson(initialSessionOpts)} then retrying it with un-executed insert statement in a command in ${
        tojson(retrySessionOpts)}`);

    const [initialRes] = runCommandsWithSessionOpts(
        [{insert: kCollName, documents: [{_id: 1}], stmtIds: [stmtId1]}], initialSessionOpts);
    const [retryRes] = runCommandsWithSessionOpts(
        [{insert: kCollName, documents: [{_id: 1}, {_id: 2}], stmtIds: [stmtId1, stmtId2]}],
        retrySessionOpts);

    assert.eq(initialRes.n, 1);
    assert(!initialRes.hasOwnProperty("retriedStmtIds"));
    assert.eq(retryRes.n, 2);
    assert.eq(retryRes.retriedStmtIds, [stmtId1]);
    assert.eq(shard0TestColl.find({_id: 1}).itcount(), 1);
    assert.eq(shard0TestColl.find({_id: 2}).itcount(), 1);

    assert.commandWorked(shard0TestColl.remove({}));
}

function runUpdateTestRetryWithAdditionalStatement(makeSessionOptionsFunc) {
    const {initialSessionOpts, retrySessionOpts} = makeSessionOptionsFunc();

    jsTest.log(`Test executing an update statement in a command in ${
        tojson(initialSessionOpts)} then retrying it with an un-executed update statement in a command in ${
        tojson(retrySessionOpts)}`);

    assert.commandWorked(shard0TestColl.insert([{_id: 1, x: 0}, {_id: 2, x: 0}]));

    const [initialRes] = runCommandsWithSessionOpts(
        [{update: kCollName, updates: [{q: {_id: 1}, u: {$inc: {x: 1}}}], stmtIds: [stmtId1]}],
        initialSessionOpts);
    const [retryRes] = runCommandsWithSessionOpts(
        [{
            update: kCollName,
            updates: [{q: {_id: 1}, u: {$inc: {x: 1}}}, {q: {_id: 2}, u: {$inc: {x: 1}}}],
            stmtIds: [stmtId1, stmtId2]
        }],
        retrySessionOpts);

    assert.eq(initialRes.nModified, 1);
    assert(!initialRes.hasOwnProperty("retriedStmtIds"));
    assert.eq(retryRes.nModified, 2);
    assert.eq(retryRes.retriedStmtIds, [stmtId1]);
    assert.eq(shard0TestColl.find({_id: 1, x: 1}).itcount(), 1);
    assert.eq(shard0TestColl.find({_id: 2, x: 1}).itcount(), 1);

    assert.commandWorked(shard0TestColl.remove({}));
}

function runDeleteTestRetryWithAdditionalStatement(makeSessionOptionsFunc) {
    const {initialSessionOpts, retrySessionOpts} = makeSessionOptionsFunc();

    jsTest.log(`Test executing a delete statement in a command in ${
        tojson(initialSessionOpts)} then retrying it with an un-executed delete statement in a command in ${
        tojson(retrySessionOpts)}`);

    assert.commandWorked(shard0TestColl.insert([{_id: 1}, {_id: 2}]));

    const [initialRes] = runCommandsWithSessionOpts(
        [{delete: kCollName, deletes: [{q: {_id: 1}, limit: 1}], stmtIds: [stmtId1]}],
        initialSessionOpts);
    const [retryRes] =
        runCommandsWithSessionOpts([{
                                       delete: kCollName,
                                       deletes: [{q: {_id: 1}, limit: 1}, {q: {_id: 2}, limit: 1}],
                                       stmtIds: [stmtId1, stmtId2]
                                   }],
                                   retrySessionOpts);

    assert.eq(initialRes.n, 1);
    assert(!initialRes.hasOwnProperty("retriedStmtIds"));
    assert.eq(retryRes.n, 2);
    assert.eq(retryRes.retriedStmtIds, [stmtId1]);
    assert.eq(shard0TestColl.find({_id: 1}).itcount(), 0);
    assert.eq(shard0TestColl.find({_id: 2}).itcount(), 0);

    assert.commandWorked(shard0TestColl.remove({}));
}

// Test that the response to a retried findAndModify command contains the "retriedStmtId" field.

function runFindAndModifyTest(makeSessionOptionsFunc) {
    const {initialSessionOpts, retrySessionOpts} = makeSessionOptionsFunc();

    jsTest.log(`Test executing a findAndModify statement in a command in ${
        tojson(initialSessionOpts)} then retrying it in a command in ${tojson(retrySessionOpts)}`);

    const cmdObj = {
        findAndModify: kCollName,
        query: {_id: 1, x: 0},
        update: {$inc: {x: 1}},
        upsert: true,
        stmtId: stmtId1
    };
    const [initialRes] = runCommandsWithSessionOpts([cmdObj], initialSessionOpts);
    const [retryRes] = runCommandsWithSessionOpts([cmdObj], retrySessionOpts);

    assert.eq(initialRes.lastErrorObject, retryRes.lastErrorObject);
    assert.eq(initialRes.value, retryRes.value);
    assert(!initialRes.hasOwnProperty("retriedStmtIds"));
    assert.eq(retryRes.retriedStmtId, stmtId1);
    assert.eq(shard0TestColl.find({_id: 1, x: 1}).itcount(), 1);

    assert.commandWorked(shard0TestColl.remove({}));
}

function runTests(makeSessionOptionsFunc) {
    runInsertTestRetryStatementsSeparately(makeSessionOptionsFunc);
    runUpdateTestRetryStatementsSeparately(makeSessionOptionsFunc);
    runDeleteTestRetryStatementsSeparately(makeSessionOptionsFunc);

    runInsertTestRetryWithAdditionalStatement(makeSessionOptionsFunc);
    runUpdateTestRetryWithAdditionalStatement(makeSessionOptionsFunc);
    runDeleteTestRetryWithAdditionalStatement(makeSessionOptionsFunc);

    runFindAndModifyTest(makeSessionOptionsFunc);
}

{
    let makeSessionOptions = () => {
        const sessionUUID = UUID();
        // Retryable writes in the parent session.
        const initialSessionOpts = {
            lsid: {id: sessionUUID},
            txnNumber: NumberLong(5),
            isTransaction: false
        };
        // Retryable writes in the parent session.
        const retrySessionOpts = {
            lsid: {id: sessionUUID},
            txnNumber: NumberLong(5),
            isTransaction: false
        };
        return {initialSessionOpts, retrySessionOpts};
    };

    runTests(makeSessionOptions);
}

{
    let makeSessionOptions = () => {
        const sessionUUID = UUID();
        // Retryable writes in the parent session.
        const initialSessionOpts = {
            lsid: {id: sessionUUID},
            txnNumber: NumberLong(5),
            isTransaction: false
        };
        // Internal transaction for retryable writes in a child session.
        const retrySessionOpts = {
            lsid: {id: sessionUUID, txnNumber: NumberLong(5), txnUUID: UUID()},
            txnNumber: NumberLong(0),
            isTransaction: true
        };
        return {initialSessionOpts, retrySessionOpts};
    };

    runTests(makeSessionOptions);
}

{
    let makeSessionOptions = () => {
        const sessionUUID = UUID();
        // Internal transaction for retryable writes in a child session.
        const initialSessionOpts = {
            lsid: {id: sessionUUID, txnNumber: NumberLong(5), txnUUID: UUID()},
            txnNumber: NumberLong(0),
            isTransaction: true
        };
        // Retryable writes in the parent session.
        const retrySessionOpts = {
            lsid: {id: sessionUUID},
            txnNumber: NumberLong(5),
            isTransaction: false
        };
        return {initialSessionOpts, retrySessionOpts};
    };

    runTests(makeSessionOptions);
}

{
    let makeSessionOptions = () => {
        const sessionUUID = UUID();
        // Internal transaction for retryable writes in a child session.
        const initialSessionOpts = {
            lsid: {id: sessionUUID, txnNumber: NumberLong(5), txnUUID: UUID()},
            txnNumber: NumberLong(0),
            isTransaction: true
        };
        // Retryable writes in the parent session.
        const retrySessionOpts = {
            lsid: {id: sessionUUID, txnNumber: NumberLong(5), txnUUID: UUID()},
            txnNumber: NumberLong(0),
            isTransaction: true
        };
        return {initialSessionOpts, retrySessionOpts};
    };

    runTests(makeSessionOptions);
}

st.stop();
})();
