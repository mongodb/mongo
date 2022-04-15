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

st.stop();
})();
