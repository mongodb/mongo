/*
 * Test that internal sessions documents are properly removed from the config.transactions
 * collection.
 */
(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");

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
    let stmtId = 0;

    // Create the parent and child sessions
    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 0}],
        lsid: parentLsid,
        txnNumber: NumberLong(4),
        stmtId: NumberInt(stmtId++)
    }));

    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: childLsid0,
        txnNumber: NumberLong(0),
        stmtId: NumberInt(stmtId++),
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
        stmtId: NumberInt(stmtId++),
        startTransaction: true,
        autocommit: false
    }));

    assert.commandWorked(testDB.adminCommand(
        {commitTransaction: 1, lsid: childLsid1, txnNumber: NumberLong(0), autocommit: false}));

    const parentDocBeforeDowngrade = shard0Primary.getCollection(kConfigTxnNs).findOne({
        "_id.id": sessionUUID,
        "_id.txnNumber": {"$exists": false}
    });

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
    let stmtId = 0;

    // Start a parent session without related children.
    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 0}],
        lsid: unrelatedParentLsid,
        txnNumber: NumberLong(10),
    }));

    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: childLsid0,
        txnNumber: NumberLong(0),
        stmtId: NumberInt(stmtId++),
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
        stmtId: NumberInt(stmtId++),
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
    let stmtId = 0;

    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: childLsid0,
        txnNumber: NumberLong(0),
        stmtId: NumberInt(stmtId++),
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
        stmtId: NumberInt(stmtId++),
        startTransaction: true,
        autocommit: false
    }));

    assert.commandWorked(testDB.adminCommand(
        {commitTransaction: 1, lsid: childLsid1, txnNumber: NumberLong(0), autocommit: false}));

    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 0}],
        lsid: parentLsid,
        txnNumber: NumberLong(13),
        stmtId: NumberInt(stmtId++)
    }));

    const parentDocBeforeDowngrade = shard0Primary.getCollection(kConfigTxnNs).findOne({
        "_id.id": sessionUUID,
        "_id.txnNumber": {"$exists": false}
    });

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
    let stmtId = 0;

    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: childLsid0,
        txnNumber: NumberLong(0),
        stmtId: NumberInt(stmtId++),
        startTransaction: true,
        autocommit: false
    }));

    assert.commandWorked(testDB.adminCommand(
        {commitTransaction: 1, lsid: childLsid0, txnNumber: NumberLong(0), autocommit: false}));

    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 0}],
        lsid: parentLsid,
        txnNumber: NumberLong(14),
        stmtId: NumberInt(stmtId++)
    }));

    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 2}],
        lsid: childLsid1,
        txnNumber: NumberLong(0),
        stmtId: NumberInt(stmtId++),
        startTransaction: true,
        autocommit: false
    }));

    assert.commandWorked(testDB.adminCommand(
        {commitTransaction: 1, lsid: childLsid1, txnNumber: NumberLong(0), autocommit: false}));

    const parentDocBeforeDowngrade = shard0Primary.getCollection(kConfigTxnNs).findOne({
        "_id.id": sessionUUID,
        "_id.txnNumber": {"$exists": false}
    });

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

(() => {
    jsTest.log(
        "Test downgrade does not modify a parent session document if it has a transaction or " +
        "retryable write with a higher txnNumber than its children that starts after setFCV has " +
        "scanned the config.transactions collection");

    const st = new ShardingTest({shards: {rs0: {nodes: 2}}});
    const shard0Rst = st.rs0;
    const shard0Primary = shard0Rst.getPrimary();

    const kDbName = "testDb";
    const kCollName = "testColl";
    const testDB = shard0Primary.getDB(kDbName);

    // Initially, the highest txnNumber for parentSession0 is 15 (retryable write with
    // executed using childSession0), and the highest txnNumber for parentSession1 is 5 (retryable
    // write executed using childSession1). Between when setFCV scans the config.transactions
    // collection to determine the highest txnNumber for each session and when the write is applied,
    // a transaction with txnNumber=16 starts and commits on parentSession0. Below we verify that
    // setFCV updates the config.transactions entry for parentSession1 but not for the one for
    // parentSession0.

    const parentLsid0 = {id: UUID()};
    let parentTxnNumber0 = 15;
    const childLsid0 = {
        id: parentLsid0.id,
        txnNumber: NumberLong(parentTxnNumber0),
        txnUUID: UUID()
    };
    const childTxnNumber0 = NumberLong(0);

    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: childLsid0,
        txnNumber: childTxnNumber0,
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false
    }));
    assert.commandWorked(testDB.adminCommand(
        {commitTransaction: 1, lsid: childLsid0, txnNumber: childTxnNumber0, autocommit: false}));

    const parentLsid1 = {id: UUID()};
    let parentTxnNumber1 = 5;
    const childLsid1 = {
        id: parentLsid1.id,
        txnNumber: NumberLong(parentTxnNumber1),
        txnUUID: UUID()
    };
    const childTxnNumber1 = NumberLong(0);

    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: childLsid1,
        txnNumber: childTxnNumber1,
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false
    }));
    assert.commandWorked(testDB.adminCommand(
        {commitTransaction: 1, lsid: childLsid1, txnNumber: childTxnNumber1, autocommit: false}));

    let runSetFCV = (shard0PrimaryHost) => {
        const shard0Primary = new Mongo(shard0PrimaryHost);
        assert.commandWorked(
            shard0Primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    };

    let fp = configureFailPoint(shard0Primary, "hangBeforeUpdatingSessionDocs");
    let setFCVThread = new Thread(runSetFCV, shard0Primary.host);
    setFCVThread.start();
    fp.wait();

    parentTxnNumber0++;
    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: parentLsid0,
        txnNumber: NumberLong(parentTxnNumber0),
        startTransaction: true,
        autocommit: false
    }));
    assert.commandWorked(testDB.adminCommand({
        commitTransaction: 1,
        lsid: parentLsid0,
        txnNumber: NumberLong(parentTxnNumber0),
        autocommit: false
    }));

    const parentDoc0BeforeDowngrade = shard0Primary.getCollection(kConfigTxnNs).findOne({
        "_id.id": parentLsid0.id,
        "_id.txnNumber": {"$exists": false}
    });
    assert.eq(parentDoc0BeforeDowngrade.txnNum, parentTxnNumber0);

    fp.off();
    setFCVThread.join();

    shard0Rst.nodes.forEach(node => {
        const docs0 = node.getCollection(kConfigTxnNs).find({"_id.id": parentLsid0.id}).toArray();
        assert.eq(docs0.length, 1);
        const parentDoc0AfterDowngrade = docs0[0];
        assert.eq(parentDoc0AfterDowngrade, parentDoc0BeforeDowngrade);

        const docs1 = node.getCollection(kConfigTxnNs).find({"_id.id": parentLsid1.id}).toArray();
        assert.eq(docs1.length, 1);
        const parentDoc1AfterDowngrade = docs1[0];
        assert.eq(parentDoc1AfterDowngrade.txnNum, parentTxnNumber1);
        assert.eq(parentDoc1AfterDowngrade.lastWriteOpTime.ts, Timestamp(1, 0));
        assert.eq(parentDoc1AfterDowngrade.lastWriteOpTime.t, NumberLong(1));
    });

    st.stop();
})();
})();
