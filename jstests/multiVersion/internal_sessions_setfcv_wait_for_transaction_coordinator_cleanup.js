/*
 * Tests that setFCV waits for transaction coordinators for internal transactions to be removed.
 *
 * @tags: [uses_transactions]
 */

(function() {
'use strict';

load('jstests/sharding/libs/sharded_transactions_helpers.js');
load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");

const kConfigTxnCoord = "config.transaction_coordinators";
const kDbName = "testDb";
const kCollName = "testColl";

const ns = kDbName + "." + kCollName;

// A monotonically increasing value to deduplicate document insertions.
let docVal = 5;

const st = new ShardingTest({shards: 2});

let coordinator = st.shard0;
let participant1 = st.shard1;

// Create a sharded collection with a chunk on each shard:
// shard0: [-inf, 0)
// shard1: [0, +inf)
assert.commandWorked(st.s.adminCommand({enableSharding: kDbName}));
assert.commandWorked(st.s.adminCommand({movePrimary: kDbName, to: coordinator.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: participant1.shardName}));

st.refreshCatalogCacheForNs(st.s, ns);

function runTestBasic(lsid) {
    jsTest.log("Test transaction coordinator documents are deleted before downgrade finishes " +
               "with lsid: " + tojson(lsid));

    // Upgrade fcv to make sure cluster is on the latestFCV before starting any transactions.
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    let commitTxnFp = configureFailPoint(coordinator, "hangBeforeCommitingTxn");
    let deleteCoordinatorDocFp =
        configureFailPoint(coordinator, "hangBeforeDeletingCoordinatorDoc");

    const txnParams = Object.assign(lsid, {kCollName: kCollName, kDbName: kDbName, val: docVal++});

    let insertDocumentsInTransaction = (host, txnParams) => {
        let conn = new Mongo(host);
        let lsid = {
            id: UUID(txnParams.sessionUUID),
        };

        if (txnParams.txnNumber) {
            Object.assign(lsid, {txnNumber: NumberLong(txnParams.txnNumber)});
        }

        if (txnParams.txnUUID) {
            Object.assign(lsid, {txnUUID: UUID(txnParams.txnUUID)});
        }

        // Inserts are split to guarantee that shard0 will be chosen as the coordinator.
        assert.commandWorked(conn.getDB(txnParams.kDbName).runCommand({
            insert: txnParams.kCollName,
            documents: [{_id: -1 * txnParams.val}],
            lsid: lsid,
            txnNumber: NumberLong(0),
            stmtId: NumberInt(0),
            startTransaction: true,
            autocommit: false,
        }));

        assert.commandWorked(conn.getDB(txnParams.kDbName).runCommand({
            insert: txnParams.kCollName,
            documents: [{_id: txnParams.val}],
            lsid: lsid,
            txnNumber: NumberLong(0),
            stmtId: NumberInt(1),
            autocommit: false,
        }));

        assert.commandWorked(conn.adminCommand(
            {commitTransaction: 1, lsid: lsid, txnNumber: NumberLong(0), autocommit: false}));
    };

    const transactionCommitThread = new Thread(insertDocumentsInTransaction, st.s.host, txnParams);
    transactionCommitThread.start();

    commitTxnFp.wait();

    let setFCVCmd = (host) => {
        let conn = new Mongo(host);
        assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    };
    const setFCVThread = new Thread(setFCVCmd, st.s.host);
    setFCVThread.start();

    commitTxnFp.off();
    deleteCoordinatorDocFp.wait();

    const coordinatorDocBeforeDelete = coordinator.getCollection(kConfigTxnCoord).findOne();
    assert.eq(coordinatorDocBeforeDelete._id.lsid.id, UUID(txnParams.sessionUUID));

    let setFCVCurrOpRes = assert.commandWorked(coordinator.adminCommand(
        {currentOp: true, "command.setFeatureCompatibilityVersion": {$exists: true}}));

    assert.neq(setFCVCurrOpRes, null);

    deleteCoordinatorDocFp.off();
    setFCVThread.join();

    const coordinatorDocAfterDelete = coordinator.getCollection(kConfigTxnCoord).findOne();
    assert.eq(coordinatorDocAfterDelete, null);

    transactionCommitThread.join();
}

function runTestWithFailoverBeforeDocumentRemoval(lsid) {
    jsTest.log("Test transaction coordinator documents are deleted before downgrade fully " +
               "completes while there is a stepdown " +
               "with lsid: " + tojson(lsid));

    // Upgrade fcv to make sure cluster is on the latestFCV before starting any transactions.
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    let commitTxnFp = configureFailPoint(coordinator, "hangBeforeCommitingTxn");
    let deleteCoordinatorDocFp =
        configureFailPoint(coordinator, "hangBeforeDeletingCoordinatorDoc");
    let fcvDowngradeFp = configureFailPoint(coordinator, "hangWhileDowngrading");

    const txnParams = Object.assign(lsid, {kCollName: kCollName, kDbName: kDbName, val: docVal++});

    let insertDocumentsInTransaction = (host, txnParams) => {
        let conn = new Mongo(host);
        let lsid = {
            id: UUID(txnParams.sessionUUID),
        };

        if (txnParams.txnNumber) {
            Object.assign(lsid, {txnNumber: NumberLong(txnParams.txnNumber)});
        }

        if (txnParams.txnUUID) {
            Object.assign(lsid, {txnUUID: UUID(txnParams.txnUUID)});
        }

        // Inserts are split to guarantee that shard0 will be chosen as the coordinator.
        assert.commandWorked(conn.getDB(txnParams.kDbName).runCommand({
            insert: txnParams.kCollName,
            documents: [{_id: -1 * txnParams.val}],
            lsid: lsid,
            txnNumber: NumberLong(0),
            stmtId: NumberInt(0),
            startTransaction: true,
            autocommit: false,
        }));

        assert.commandWorked(conn.getDB(txnParams.kDbName).runCommand({
            insert: txnParams.kCollName,
            documents: [{_id: txnParams.val}],
            lsid: lsid,
            txnNumber: NumberLong(0),
            stmtId: NumberInt(1),
            autocommit: false,
        }));

        assert.commandWorked(conn.adminCommand(
            {commitTransaction: 1, lsid: lsid, txnNumber: NumberLong(0), autocommit: false}));
    };

    const transactionCommitThread = new Thread(insertDocumentsInTransaction, st.s.host, txnParams);
    transactionCommitThread.start();

    commitTxnFp.wait();

    let setFCVCmd = (host) => {
        // setFCV should get interrupted with a transient error and successfully retry.
        let conn = new Mongo(host);
        assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    };

    const setFCVThread = new Thread(setFCVCmd, st.s.host);
    setFCVThread.start();

    commitTxnFp.off();
    deleteCoordinatorDocFp.wait();

    const coordinatorDocBeforeDelete = coordinator.getCollection(kConfigTxnCoord).findOne();
    assert.eq(coordinatorDocBeforeDelete._id.lsid.id, UUID(txnParams.sessionUUID));

    let setFCVCurrOpRes = assert.commandWorked(coordinator.adminCommand(
        {currentOp: true, "command.setFeatureCompatibilityVersion": {$exists: true}}));
    assert.neq(setFCVCurrOpRes, null);

    let primary = coordinator.rs.getPrimary();
    assert.commandWorked(
        primary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
    assert.commandWorked(primary.adminCommand({replSetFreeze: 0}));

    fcvDowngradeFp.off();
    deleteCoordinatorDocFp.off();
    setFCVThread.join();

    const coordinatorDocAfterDelete = coordinator.getCollection(kConfigTxnCoord).findOne();
    assert.eq(coordinatorDocAfterDelete, null);

    transactionCommitThread.join();
}

const testFuncs = [
    runTestBasic,
    runTestWithFailoverBeforeDocumentRemoval,
];

for (const testFunc of testFuncs) {
    const lsidCombinations = [
        {
            sessionUUID: extractUUIDFromObject(UUID()),
            txnUUID: extractUUIDFromObject(UUID()),
        },
        {
            sessionUUID: extractUUIDFromObject(UUID()),
            txnUUID: extractUUIDFromObject(UUID()),
            txnNumber: NumberLong(0)
        }
    ];

    for (const lsid of lsidCombinations) {
        testFunc(lsid);
    }
}

st.stop();
})();
