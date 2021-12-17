/*
 * Test that internal sessions documents are properly removed from the config.transactions
 * collection.
 *
 * @tags: [featureFlagInternalTransactions]
 */
(function() {
'use strict';

const kConfigTxnNs = "config.transactions";
const kDbName = "testDb";
const kCollName = "testColl";

(() => {
    jsTest.log(
        "Test downgrade updates existing parent session document with highest txnNumber of its " +
        "child sessions");

    const st = new ShardingTest({shards: {rs0: {nodes: 2}}});
    const shard0Rst = st.rs0;
    const shard0Primary = shard0Rst.getPrimary();

    const testDB = shard0Primary.getDB(kDbName);

    const sessionUUID = UUID();
    const parentLsid = {id: sessionUUID};
    const childLsid0 = {id: sessionUUID, txnNumber: NumberLong(5), txnUUID: UUID()};
    const childLsid1 = {id: sessionUUID, txnNumber: NumberLong(6), txnUUID: UUID()};

    // Create the parent and child sessions
    assert.commandWorked(testDB.runCommand(
        {insert: kCollName, documents: [{x: 0}], lsid: parentLsid, txnNumber: NumberLong(4)}));

    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: childLsid0,
        txnNumber: NumberLong(0),
        startTransaction: true,
        autocommit: false
    }));

    assert.commandWorked(testDB.adminCommand(
        {commitTransaction: 1, lsid: childLsid0, txnNumber: NumberLong(0), autocommit: false}));

    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 2}],
        lsid: childLsid1,
        txnNumber: NumberLong(0),
        startTransaction: true,
        autocommit: false
    }));

    assert.commandWorked(testDB.adminCommand(
        {commitTransaction: 1, lsid: childLsid1, txnNumber: NumberLong(0), autocommit: false}));

    const parentDocBeforeDowngrade =
        shard0Primary.getCollection(kConfigTxnNs).findOne({"_id.id": sessionUUID});

    assert.commandWorked(shard0Primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

    shard0Rst.nodes.forEach(node => {
        // We expect every document except for the parent session documents to be deleted.
        const collectionDocCount = node.getCollection(kConfigTxnNs).count();
        assert.eq(collectionDocCount, 1);

        const doc = node.getCollection(kConfigTxnNs).findOne({"_id.id": sessionUUID});
        assert.eq(doc.txnNum, NumberLong(6));
        assert.eq(doc.lastWriteOpTime.ts, Timestamp(1, 0));
        assert.eq(doc.lastWriteOpTime.t, NumberLong(1));
        assert.gte(doc.lastWriteDate, parentDocBeforeDowngrade.lastWriteDate);
    });

    st.stop();
})();

(() => {
    jsTest.log(
        "Test downgrade upserts new transaction document if relevant parent session document " +
        "does not exist and does not modify unrelated transaction documents.");

    const st = new ShardingTest({shards: {rs0: {nodes: 2}}});
    const shard0Rst = st.rs0;
    const shard0Primary = shard0Rst.getPrimary();

    const kDbName = "testDb";
    const kCollName = "testColl";
    const testDB = shard0Primary.getDB(kDbName);

    const sessionUUID = UUID();
    const unrelatedParentLsid = {id: UUID()};
    const childLsid0 = {id: sessionUUID, txnNumber: NumberLong(7), txnUUID: UUID()};
    const childLsid1 = {id: sessionUUID, txnNumber: NumberLong(8), txnUUID: UUID()};

    // Start a parent session without related children.
    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 0}],
        lsid: unrelatedParentLsid,
        txnNumber: NumberLong(10)
    }));

    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: childLsid0,
        txnNumber: NumberLong(0),
        startTransaction: true,
        autocommit: false
    }));

    assert.commandWorked(testDB.adminCommand(
        {commitTransaction: 1, lsid: childLsid0, txnNumber: NumberLong(0), autocommit: false}));

    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 2}],
        lsid: childLsid1,
        txnNumber: NumberLong(0),
        startTransaction: true,
        autocommit: false
    }));

    assert.commandWorked(testDB.adminCommand(
        {commitTransaction: 1, lsid: childLsid1, txnNumber: NumberLong(0), autocommit: false}));

    const unrelatedParentDocBeforeDowngrade =
        shard0Primary.getCollection(kConfigTxnNs).findOne({"_id.id": unrelatedParentLsid.id});

    assert.commandWorked(shard0Primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

    shard0Rst.nodes.forEach(node => {
        // We expect every document except for the parent session documents to be deleted.
        const collectionDocCount = node.getCollection(kConfigTxnNs).count();
        assert.eq(collectionDocCount, 2);

        const upsertedDoc = node.getCollection(kConfigTxnNs).findOne({"_id.id": sessionUUID});
        assert.eq(upsertedDoc.txnNum, NumberLong(8));
        assert.eq(upsertedDoc.lastWriteOpTime.ts, Timestamp(1, 0));
        assert.eq(upsertedDoc.lastWriteOpTime.t, NumberLong(1));

        const unrelatedParentDocAfterDowngrade =
            node.getCollection(kConfigTxnNs).findOne({"_id.id": unrelatedParentLsid.id});
        assert.eq(unrelatedParentDocAfterDowngrade, unrelatedParentDocBeforeDowngrade);

        // For newly upserted session documents, we use the current wall clock time as the
        // lastWriteDate, thus we expect it to be farther ahead than the unchanged session document.
        assert.gte(upsertedDoc.lastWriteDate, unrelatedParentDocAfterDowngrade.lastWriteDate);
    });

    st.stop();
})();

(() => {
    jsTest.log(
        "Test downgrade does not modify a parent session document if it has a higher txnNumber " +
        "than its children.");

    const st = new ShardingTest({shards: {rs0: {nodes: 2}}});
    const shard0Rst = st.rs0;
    const shard0Primary = shard0Rst.getPrimary();

    const kDbName = "testDb";
    const kCollName = "testColl";
    const testDB = shard0Primary.getDB(kDbName);

    const sessionUUID = UUID();
    const parentLsid = {id: sessionUUID};
    const childLsid0 = {id: sessionUUID, txnNumber: NumberLong(11), txnUUID: UUID()};
    const childLsid1 = {id: sessionUUID, txnNumber: NumberLong(12), txnUUID: UUID()};

    // Start a parent session without related children.
    assert.commandWorked(testDB.runCommand(
        {insert: kCollName, documents: [{x: 0}], lsid: parentLsid, txnNumber: NumberLong(13)}));

    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: childLsid0,
        txnNumber: NumberLong(0),
        startTransaction: true,
        autocommit: false
    }));

    assert.commandWorked(testDB.adminCommand(
        {commitTransaction: 1, lsid: childLsid0, txnNumber: NumberLong(0), autocommit: false}));

    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 2}],
        lsid: childLsid1,
        txnNumber: NumberLong(0),
        startTransaction: true,
        autocommit: false
    }));

    const parentDocBeforeDowngrade =
        shard0Primary.getCollection(kConfigTxnNs).findOne({"_id.id": sessionUUID});

    assert.commandWorked(testDB.adminCommand(
        {commitTransaction: 1, lsid: childLsid1, txnNumber: NumberLong(0), autocommit: false}));

    assert.commandWorked(shard0Primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

    shard0Rst.nodes.forEach(node => {
        // We expect every document except for the parent session documents to be deleted.
        const collectionDocCount = node.getCollection(kConfigTxnNs).count();
        assert.eq(collectionDocCount, 1);

        const parentDocAfterDowngrade =
            shard0Primary.getCollection(kConfigTxnNs).findOne({"_id.id": sessionUUID});
        assert.eq(parentDocAfterDowngrade, parentDocBeforeDowngrade);
    });

    st.stop();
})();

(() => {
    jsTest.log("Test downgrade modifies a parent session document if it has a txnNumber equal to " +
               " the highest txnNumber of its children.");

    const st = new ShardingTest({shards: {rs0: {nodes: 2}}});
    const shard0Rst = st.rs0;
    const shard0Primary = shard0Rst.getPrimary();

    const kDbName = "testDb";
    const kCollName = "testColl";
    const testDB = shard0Primary.getDB(kDbName);

    const sessionUUID = UUID();
    const parentLsid = {id: sessionUUID};
    const childLsid0 = {id: sessionUUID, txnNumber: NumberLong(13), txnUUID: UUID()};
    const childLsid1 = {id: sessionUUID, txnNumber: NumberLong(14), txnUUID: UUID()};

    // Start a parent session without related children.
    assert.commandWorked(testDB.runCommand(
        {insert: kCollName, documents: [{x: 0}], lsid: parentLsid, txnNumber: NumberLong(14)}));

    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: childLsid0,
        txnNumber: NumberLong(0),
        startTransaction: true,
        autocommit: false
    }));

    assert.commandWorked(testDB.adminCommand(
        {commitTransaction: 1, lsid: childLsid0, txnNumber: NumberLong(0), autocommit: false}));

    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 2}],
        lsid: childLsid1,
        txnNumber: NumberLong(0),
        startTransaction: true,
        autocommit: false
    }));

    assert.commandWorked(testDB.adminCommand(
        {commitTransaction: 1, lsid: childLsid1, txnNumber: NumberLong(0), autocommit: false}));

    const parentDocBeforeDowngrade =
        shard0Primary.getCollection(kConfigTxnNs).findOne({"_id.id": sessionUUID});

    assert.commandWorked(shard0Primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

    shard0Rst.nodes.forEach(node => {
        // We expect every document except for the parent session documents to be deleted.
        const collectionDocCount = node.getCollection(kConfigTxnNs).count();
        assert.eq(collectionDocCount, 1);

        const doc = node.getCollection(kConfigTxnNs).findOne({"_id.id": sessionUUID});
        assert.eq(doc.txnNum, NumberLong(14));
        assert.eq(doc.lastWriteOpTime.ts, Timestamp(1, 0));
        assert.eq(doc.lastWriteOpTime.t, NumberLong(1));
        assert.gte(doc.lastWriteDate, parentDocBeforeDowngrade.lastWriteDate);
    });

    st.stop();
})();
})();
