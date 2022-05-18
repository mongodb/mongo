/**
 * Tests that the config.transactions collection has a partial index such that the find query of the
 * form {"parentLsid": ..., "_id.txnNumber": ...} is a covered query.
 *
 * @tags: [requires_fcv_60, uses_transactions]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");

const st = new ShardingTest({shards: {rs0: {nodes: 2}}});

const kDbName = "testDb";
const kCollName = "testColl";
const kConfigTxnNs = "config.transactions";

const mongosTestDB = st.s.getDB(kDbName);
const shard0PrimaryConfigTxnColl = st.rs0.getPrimary().getCollection(kConfigTxnNs);

const sessionUUID = UUID();
const parentLsid = {
    id: sessionUUID
};
const parentTxnNumber = 35;
let stmtId = 0;

assert.commandWorked(mongosTestDB.runCommand({
    insert: kCollName,
    documents: [{_id: 0}],
    lsid: parentLsid,
    txnNumber: NumberLong(parentTxnNumber),
    stmtId: NumberInt(stmtId++)
}));
const parentSessionDoc = shard0PrimaryConfigTxnColl.findOne({"_id.id": sessionUUID});

const childLsid = {
    id: sessionUUID,
    txnNumber: NumberLong(parentTxnNumber),
    txnUUID: UUID()
};
let childTxnNumber = 0;

function runRetryableInternalTransaction(txnNumber) {
    assert.commandWorked(mongosTestDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: childLsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(stmtId++),
        autocommit: false,
        startTransaction: true
    }));
    assert.commandWorked(mongosTestDB.adminCommand({
        commitTransaction: 1,
        lsid: childLsid,
        txnNumber: NumberLong(txnNumber),
        autocommit: false
    }));
}

function assertPartialIndexExists(node) {
    const configDB = node.getDB("config");
    const indexSpecs = assert.commandWorked(configDB.runCommand({"listIndexes": "transactions"}))
                           .cursor.firstBatch;
    indexSpecs.sort((index0, index1) => index0.name > index1.name);
    assert.eq(indexSpecs.length, 2);
    const idIndexSpec = indexSpecs[0];
    assert.eq(idIndexSpec.key, {"_id": 1});
    const partialIndexSpec = indexSpecs[1];
    assert.eq(partialIndexSpec.key, {"parentLsid": 1, "_id.txnNumber": 1, "_id": 1});
    assert.eq(partialIndexSpec.partialFilterExpression, {"parentLsid": {"$exists": true}});
}

function assertFindUsesCoveredQuery(node) {
    const configTxnColl = node.getCollection(kConfigTxnNs);
    const childSessionDoc = configTxnColl.findOne({
        "_id.id": sessionUUID,
        "_id.txnNumber": childLsid.txnNumber,
        "_id.txnUUID": childLsid.txnUUID
    });

    const explainRes = assert.commandWorked(
        configTxnColl.explain()
            .find({"parentLsid": parentSessionDoc._id, "_id.txnNumber": childLsid.txnNumber},
                  {_id: 1})
            .finish());
    const winningPlan = getWinningPlan(explainRes.queryPlanner);
    assert.eq(winningPlan.stage, "PROJECTION_COVERED");
    assert.eq(winningPlan.inputStage.stage, "IXSCAN");

    const findRes =
        configTxnColl
            .find({"parentLsid": parentSessionDoc._id, "_id.txnNumber": childLsid.txnNumber},
                  {_id: 1})
            .toArray();
    assert.eq(findRes.length, 1);
    assert.eq(findRes[0]._id, childSessionDoc._id);
}

runRetryableInternalTransaction(childTxnNumber);
assert.eq(shard0PrimaryConfigTxnColl.count({"_id.id": sessionUUID}), 2);

st.rs0.nodes.forEach(node => {
    assertPartialIndexExists(node);
    assertFindUsesCoveredQuery(node);
});

childTxnNumber++;
runRetryableInternalTransaction(childTxnNumber);
assert.eq(shard0PrimaryConfigTxnColl.count({"_id.id": sessionUUID}), 2);

st.rs0.nodes.forEach(node => {
    assertPartialIndexExists(node);
    assertFindUsesCoveredQuery(node);
});

//
// Verify clients can create the index only if they provide the exact specification and that
// operations requiring the index fails if it does not exist.
//

const indexConn = st.rs0.getPrimary();
assert.commandWorked(indexConn.getCollection("config.transactions").dropIndex("parent_lsid"));

// Normal writes don't involve config.transactions, so they succeed.
assert.commandWorked(indexConn.getDB(kDbName).runCommand(
    {insert: kCollName, documents: [{x: 1}], lsid: {id: UUID()}}));

// Retryable writes read from the partial index, so they fail.
let res = assert.commandFailedWithCode(
    indexConn.getDB(kDbName).runCommand(
        {insert: kCollName, documents: [{x: 1}], lsid: {id: UUID()}, txnNumber: NumberLong(11)}),
    ErrorCodes.BadValue);
assert(res.errmsg.includes("Please create an index directly "), tojson(res));

// User transactions read from the partial index, so they fail.
assert.commandFailedWithCode(indexConn.getDB(kDbName).runCommand({
    insert: kCollName,
    documents: [{x: 1}],
    lsid: {id: UUID()},
    txnNumber: NumberLong(11),
    startTransaction: true,
    autocommit: false
}),
                             ErrorCodes.BadValue);

// Non retryable internal transactions do not read from or update the partial index, so they can
// succeed without the index existing.
let nonRetryableTxnSession = {id: UUID(), txnUUID: UUID()};
assert.commandWorked(indexConn.getDB(kDbName).runCommand({
    insert: kCollName,
    documents: [{x: 1}],
    lsid: nonRetryableTxnSession,
    txnNumber: NumberLong(11),
    stmtId: NumberInt(0),
    startTransaction: true,
    autocommit: false
}));
assert.commandWorked(indexConn.adminCommand({
    commitTransaction: 1,
    lsid: nonRetryableTxnSession,
    txnNumber: NumberLong(11),
    autocommit: false
}));

// Retryable transactions read from the partial index, so they fail.
assert.commandFailedWithCode(indexConn.getDB(kDbName).runCommand({
    insert: kCollName,
    documents: [{x: 1}],
    lsid: {id: UUID(), txnUUID: UUID(), txnNumber: NumberLong(2)},
    txnNumber: NumberLong(11),
    stmtId: NumberInt(0),
    startTransaction: true,
    autocommit: false
}),
                             ErrorCodes.BadValue);

// Recreating the partial index requires the exact options used internally, but in any order.
assert.commandFailedWithCode(indexConn.getDB("config").runCommand({
    createIndexes: "transactions",
    indexes: [{v: 2, name: "parent_lsid", key: {parentLsid: 1, "_id.txnNumber": 1, _id: 1}}],
}),
                             ErrorCodes.IllegalOperation);
assert.commandWorked(indexConn.getDB("config").runCommand({
    createIndexes: "transactions",
    indexes: [{
        name: "parent_lsid",
        key: {parentLsid: 1, "_id.txnNumber": 1, _id: 1},
        partialFilterExpression: {parentLsid: {$exists: true}},
        v: 2,
    }],
}));

// Operations involving the index should succeed now.

assert.commandWorked(indexConn.getDB(kDbName).runCommand(
    {insert: kCollName, documents: [{x: 1}], lsid: {id: UUID()}}));

assert.commandWorked(indexConn.getDB(kDbName).runCommand(
    {insert: kCollName, documents: [{x: 1}], lsid: {id: UUID()}, txnNumber: NumberLong(11)}));

let userSessionAfter = {id: UUID()};
assert.commandWorked(indexConn.getDB(kDbName).runCommand({
    insert: kCollName,
    documents: [{x: 1}],
    lsid: userSessionAfter,
    txnNumber: NumberLong(11),
    startTransaction: true,
    autocommit: false
}));
assert.commandWorked(indexConn.adminCommand(
    {commitTransaction: 1, lsid: userSessionAfter, txnNumber: NumberLong(11), autocommit: false}));

let nonRetryableTxnSessionAfter = {id: UUID(), txnUUID: UUID()};
assert.commandWorked(indexConn.getDB(kDbName).runCommand({
    insert: kCollName,
    documents: [{x: 1}],
    lsid: nonRetryableTxnSessionAfter,
    txnNumber: NumberLong(11),
    stmtId: NumberInt(0),
    startTransaction: true,
    autocommit: false
}));
assert.commandWorked(indexConn.adminCommand({
    commitTransaction: 1,
    lsid: nonRetryableTxnSessionAfter,
    txnNumber: NumberLong(11),
    autocommit: false
}));

let retryableTxnSessionAfter = {id: UUID(), txnUUID: UUID(), txnNumber: NumberLong(2)};
assert.commandWorked(indexConn.getDB(kDbName).runCommand({
    insert: kCollName,
    documents: [{x: 1}],
    lsid: retryableTxnSessionAfter,
    txnNumber: NumberLong(11),
    stmtId: NumberInt(0),
    startTransaction: true,
    autocommit: false
}));
assert.commandWorked(indexConn.adminCommand({
    commitTransaction: 1,
    lsid: retryableTxnSessionAfter,
    txnNumber: NumberLong(11),
    autocommit: false
}));

st.stop();
})();
