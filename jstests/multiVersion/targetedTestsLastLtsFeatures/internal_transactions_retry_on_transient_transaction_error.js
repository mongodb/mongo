/*
 * Tests that the client can only retry a transaction that failed with a transient transaction error
 * by attaching a higher txnRetryCounter when the FCV is latest.
 */
(function() {
'use strict';

function runTest(downgradeFCV) {
    load("jstests/libs/fail_point_util.js");

    const st = new ShardingTest({shards: 1, rs: {nodes: 3}});
    const shard0Rst = st.rs0;
    const shard0Primary = shard0Rst.getPrimary();

    const kDbName = "testDb";
    const kCollName = "testColl";
    const kNs = kDbName + "." + kCollName;
    const testDB = shard0Primary.getDB(kDbName);

    const kConfigTxnNs = "config.transactions";
    const kOplogNs = "local.oplog.rs";

    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: downgradeFCV}));

    jsTest.log(
        "Verify the config.transactions doc does not contain the txnRetryCounter if the FCV is not latest");
    const lsid0 = {id: UUID()};
    const txnNumber0 = NumberLong(0);
    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 0}],
        lsid: lsid0,
        txnNumber: txnNumber0,
        startTransaction: true,
        autocommit: false,
    }));
    assert.commandWorked(testDB.adminCommand({
        commitTransaction: 1,
        lsid: lsid0,
        txnNumber: txnNumber0,
        autocommit: false,
    }));
    shard0Rst.awaitReplication();
    shard0Rst.nodes.forEach(node => {
        const txnDoc = node.getCollection(kConfigTxnNs).findOne({"_id.id": lsid0.id});
        assert.neq(null, txnDoc);
        assert(!txnDoc.hasOwnProperty("txnRetryCounter"), txnDoc);
        const oplogEntry = node.getCollection(kOplogNs).findOne({"lsid.id": lsid0.id});
        assert.neq(null, oplogEntry);
        assert(!oplogEntry.hasOwnProperty("txnRetryCounter"), txnDoc);
    });

    jsTest.log("Verify retries only work in FCV latest");
    const lsid1 = {id: UUID()};
    const txnNumber1 = NumberLong(1);
    const insertCmdObj = {
        insert: kCollName,
        documents: [{x: 1}],
        lsid: lsid1,
        txnNumber: txnNumber1,
        startTransaction: true,
        autocommit: false,
        txnRetryCounter: NumberInt(1)
    };
    assert.commandFailedWithCode(testDB.runCommand(insertCmdObj),
                                 ErrorCodes.TxnRetryCounterNotSupported);

    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
    insertCmdObj.txnRetryCounter = NumberInt(2);
    assert.commandWorked(testDB.runCommand(insertCmdObj));
    assert.commandWorked(testDB.adminCommand({
        commitTransaction: 1,
        lsid: lsid1,
        txnNumber: txnNumber1,
        autocommit: false,
        txnRetryCounter: insertCmdObj.txnRetryCounter
    }));

    jsTest.log(
        "Verify the config.transactions doc contains the txnRetryCounter since the FCV is latest");
    shard0Rst.awaitReplication();
    shard0Rst.nodes.forEach(node => {
        const txnDoc = node.getCollection(kConfigTxnNs).findOne({"_id.id": lsid1.id});
        assert.neq(null, txnDoc);
        assert.eq(insertCmdObj.txnRetryCounter, txnDoc.txnRetryCounter, txnDoc);
        const oplogEntry = node.getCollection(kOplogNs).findOne({"lsid.id": lsid1.id});
        assert.neq(null, oplogEntry);
        assert.eq(insertCmdObj.txnRetryCounter, oplogEntry.txnRetryCounter, oplogEntry);
    });

    jsTest.log("Verify that the config.transactions doc does not contain txnRetryCounter if " +
               "txnRetryCounter is still the default value");

    const lsid2 = {id: UUID()};
    const txnNumber2 = NumberLong(2);
    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 2}],
        lsid: lsid2,
        txnNumber: txnNumber2,
        txnRetryCounter: NumberInt(0),
        startTransaction: true,
        autocommit: false,
    }));
    assert.commandWorked(testDB.adminCommand(
        {commitTransaction: 1, lsid: lsid2, txnNumber: txnNumber2, autocommit: false}));

    shard0Rst.awaitReplication();
    shard0Rst.nodes.forEach(node => {
        const txnDoc = node.getCollection(kConfigTxnNs).findOne({"_id.id": lsid2.id});
        assert.neq(null, txnDoc);
        assert.eq(null, txnDoc.txnRetryCounter, txnDoc);
        const oplogEntry = node.getCollection(kOplogNs).findOne({"lsid.id": lsid2.id});
        assert.neq(null, oplogEntry);
        assert.eq(null, oplogEntry.txnRetryCounter, oplogEntry);
    });

    st.stop();
}

runFeatureFlagMultiversionTest('featureFlagInternalTransactions', runTest);
})();
