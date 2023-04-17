/*
 * Tests basic support for internal sessions.
 *
 * @tags: [requires_fcv_70, uses_transactions]
 */
(function() {
'use strict';

TestData.disableImplicitSessions = true;

const st = new ShardingTest({
    shards: 1,
    mongosOptions: {
        setParameter:
            {maxSessions: 1, 'failpoint.skipClusterParameterRefresh': "{'mode':'alwaysOn'}"}
    },
    // The config server uses a session for internal operations, so raise the limit by 1 for a
    // config shard.
    shardOptions: {setParameter: {maxSessions: TestData.configShard ? 2 : 1}}
});
const shard0Primary = st.rs0.getPrimary();

const kDbName = "testDb";
const kCollName = "testColl";
const testDB = st.s.getDB(kDbName);

const kConfigTxnNs = "config.transactions";
const kConfigSessionNs = "config.system.sessions";

(() => {
    // Verify that internal sessions are only supported in transactions.
    const sessionUUID = UUID();

    jsTest.log("Test running an internal session with lsid containing txnNumber and " +
               "txnUUID outside transaction");
    const lsid1 = {id: sessionUUID, txnNumber: NumberLong(35), txnUUID: UUID()};
    assert.commandFailedWithCode(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: lsid1,
    }),
                                 ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: lsid1,
        txnNumber: NumberLong(0),
    }),
                                 ErrorCodes.InvalidOptions);

    jsTest.log(
        "Test running an internal session with with lsid containing txnUUID outside transaction");
    const lsid2 = {id: sessionUUID, txnUUID: UUID()};
    assert.commandFailedWithCode(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 2}],
        lsid: lsid1,
    }),
                                 ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 2}],
        lsid: lsid2,
        txnNumber: NumberLong(35),
    }),
                                 ErrorCodes.InvalidOptions);

    assert.eq(0, shard0Primary.getCollection(kConfigTxnNs).count({"_id.id": sessionUUID}));
    assert.commandWorked(shard0Primary.adminCommand({refreshLogicalSessionCacheNow: 1}));
    assert.eq(0, shard0Primary.getCollection(kConfigSessionNs).count({"_id.id": sessionUUID}));
})();

(() => {
    jsTest.log(
        "Test that the only supported child lsid formats are txnNumber+txnUUID, and txnUUID");

    const sessionUUID = UUID();

    assert.commandFailedWithCode(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: {id: sessionUUID, txnNumber: NumberLong(35)},
        txnNumber: NumberLong(0),
        startTransaction: true,
        autocommit: false
    }),
                                 ErrorCodes.InvalidOptions);

    assert.eq(0, shard0Primary.getCollection(kConfigTxnNs).count({"_id.id": sessionUUID}));
    assert.commandWorked(shard0Primary.adminCommand({refreshLogicalSessionCacheNow: 1}));
    assert.eq(0, shard0Primary.getCollection(kConfigSessionNs).count({"_id.id": sessionUUID}));
})();

(() => {
    // Verify that parent and child sessions are tracked using different config.transactions
    // documents but are tracked as one logical session (i.e. using the same config.system.sessions
    // document).
    const sessionUUID = UUID();

    if (TestData.configShard) {
        // Create the collection first separately, otherwise the session will be used for the
        // transaction that creates the collection, leading to one extra transaction document.
        assert.commandWorked(testDB.createCollection(kCollName));
    }

    const parentLsid = {id: sessionUUID};
    assert.commandWorked(testDB.runCommand(
        {insert: kCollName, documents: [{x: 0}], lsid: parentLsid, txnNumber: NumberLong(0)}));
    const parentSessionDoc =
        shard0Primary.getCollection(kConfigTxnNs).findOne({"_id.id": sessionUUID});
    assert.neq(parentSessionDoc, null);
    assert(!parentSessionDoc.hasOwnProperty("parentLsid"));

    const minLastUse = new Date();
    sleep(1000);

    // Starting an unrelated session should fail since the cache size is 1.
    assert.commandFailedWithCode(
        testDB.runCommand(
            {insert: kCollName, documents: [{x: 0}], lsid: {id: UUID()}, txnNumber: NumberLong(0)}),
        ErrorCodes.TooManyLogicalSessions);

    // Starting child sessions should succeed since parent and child sessions are tracked as one
    // logical session.
    jsTest.log("Test running an internal transaction with lsid containing txnNumber and txnUUID");
    const childLsid1 = {id: sessionUUID, txnNumber: NumberLong(35), txnUUID: UUID()};
    const txnNumber1 = NumberLong(0);
    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: childLsid1,
        txnNumber: txnNumber1,
        startTransaction: true,
        autocommit: false
    }));
    assert.commandWorked(testDB.adminCommand(
        {commitTransaction: 1, lsid: childLsid1, txnNumber: txnNumber1, autocommit: false}));
    const childSessionDoc1 = shard0Primary.getCollection(kConfigTxnNs).findOne({
        "_id.id": sessionUUID,
        "_id.txnNumber": childLsid1.txnNumber,
        "_id.txnUUID": childLsid1.txnUUID
    });
    assert.neq(childSessionDoc1, null);
    assert.eq(childSessionDoc1.parentLsid,
              {id: childSessionDoc1._id.id, uid: childSessionDoc1._id.uid});

    jsTest.log("Test running an internal transaction with lsid containing txnUUID");
    const childLsid2 = {id: sessionUUID, txnUUID: UUID()};
    const txnNumber2 = NumberLong(35);
    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 2}],
        lsid: childLsid2,
        txnNumber: txnNumber2,
        startTransaction: true,
        autocommit: false
    }));
    assert.commandWorked(testDB.adminCommand(
        {commitTransaction: 1, lsid: childLsid2, txnNumber: txnNumber2, autocommit: false}));
    const childSessionDoc2 = shard0Primary.getCollection(kConfigTxnNs).findOne({
        "_id.id": sessionUUID,
        "_id.txnUUID": childLsid2.txnUUID
    });
    assert.neq(childSessionDoc2, null);
    assert(!childSessionDoc2.hasOwnProperty("parentLsid"));

    assert.eq(3, shard0Primary.getCollection(kConfigTxnNs).count({"_id.id": sessionUUID}));
    assert.commandWorked(shard0Primary.adminCommand({refreshLogicalSessionCacheNow: 1}));
    const sessionDocs =
        shard0Primary.getCollection(kConfigSessionNs).find({"_id.id": sessionUUID}).toArray();
    assert.eq(sessionDocs.length, 1);
    assert(!sessionDocs[0]._id.hasOwnProperty("txnTxnNumber"), tojson(sessionDocs[0]));
    assert(!sessionDocs[0]._id.hasOwnProperty("txnUUID"), tojson(sessionDocs[0]));
    assert.gte(sessionDocs[0].lastUse, minLastUse, tojson(sessionDocs[0]));
})();

st.stop();
})();
