/**
 * Tests that the config.transactions collection has a partial index such that the find query of the
 * form {"parentLsid": ..., "_id.txnNumber": ...} is a covered query.
 *
 * @tags: [requires_fcv_60, uses_transactions]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");

const kDbName = "testDb";
const kCollName = "testColl";
const kConfigTxnNs = "config.transactions";
const kPartialIndexName = "parent_lsid";

function runTest(st, alwaysCreateFeatureFlagEnabled) {
    const mongosTestDB = st.s.getDB(kDbName);
    const shard0PrimaryConfigTxnColl = st.rs0.getPrimary().getCollection(kConfigTxnNs);

    function assertPartialIndexExists(node) {
        const configDB = node.getDB("config");
        const indexSpecs =
            assert.commandWorked(configDB.runCommand({"listIndexes": "transactions"}))
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

    function assertPartialIndexDoesNotExist(node) {
        const configDB = node.getDB("config");
        const indexSpecs =
            assert.commandWorked(configDB.runCommand({"listIndexes": "transactions"}))
                .cursor.firstBatch;
        assert.eq(indexSpecs.length, 1);
        const idIndexSpec = indexSpecs[0];
        assert.eq(idIndexSpec.key, {"_id": 1});
    }

    function indexRecreationTest(expectRecreateAfterDrop) {
        st.rs0.getPrimary().getCollection(kConfigTxnNs).dropIndex(kPartialIndexName);
        st.rs0.awaitReplication();

        st.rs0.nodes.forEach(node => {
            assertPartialIndexDoesNotExist(node);
        });

        let primary = st.rs0.getPrimary();
        assert.commandWorked(
            primary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
        assert.commandWorked(primary.adminCommand({replSetFreeze: 0}));

        st.rs0.awaitNodesAgreeOnPrimary();
        st.rs0.awaitReplication();

        st.rs0.nodes.forEach(node => {
            if (expectRecreateAfterDrop) {
                assertPartialIndexExists(node);
            } else {
                assertPartialIndexDoesNotExist(node);
            }
        });
    }

    if (TestData.configShard) {
        // A config server does internal txns, clear the transaction table to make sure it's
        // empty before dropping the index, otherwise it can't be recreated automatically.

        // Complete the setup by explicitly creating the test db (which is done through an internal
        // txn).
        assert.commandWorked(st.s.adminCommand({enableSharding: kDbName}));

        // Disable implicit sessions we can directly write to config.transactions.
        TestData.disableImplicitSessions = true;
        assert.commandWorked(st.rs0.getPrimary().getCollection(kConfigTxnNs).remove({}));
        TestData.disableImplicitSessions = false;
    }

    // If the collection is empty and the index does not exist, we should always create the partial
    // index on stepup,
    indexRecreationTest(true /* expectRecreateAfterDrop */);

    const sessionUUID = UUID();
    const parentLsid = {id: sessionUUID};
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

    const childLsid = {id: sessionUUID, txnNumber: NumberLong(parentTxnNumber), txnUUID: UUID()};
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
    assert.commandWorked(
        indexConn.getCollection("config.transactions").dropIndex(kPartialIndexName));

    // Normal writes don't involve config.transactions, so they succeed.
    assert.commandWorked(indexConn.getDB(kDbName).runCommand(
        {insert: kCollName, documents: [{x: 1}], lsid: {id: UUID()}}));

    // Retryable writes read from the partial index, so they fail.
    let res = assert.commandFailedWithCode(indexConn.getDB(kDbName).runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: {id: UUID()},
        txnNumber: NumberLong(11)
    }),
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
    assert.commandWorked(indexConn.adminCommand({
        commitTransaction: 1,
        lsid: userSessionAfter,
        txnNumber: NumberLong(11),
        autocommit: false
    }));

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

    if (!alwaysCreateFeatureFlagEnabled) {
        // We expect that if the partial index is dropped when the collection isn't empty, then on
        // stepup we should not recreate the collection.
        indexRecreationTest(false /* expectRecreateAfterDrop */);
    } else {
        // Creating the partial index when the collection isn't empty can be enabled by a feature
        // flag.
        indexRecreationTest(true /* expectRecreateAfterDrop */);
    }
}

{
    const st = new ShardingTest({shards: {rs0: {nodes: 2}}});
    runTest(st, false /* alwaysCreateFeatureFlagEnabled */);
    st.stop();
}

{
    const featureFlagSt = new ShardingTest({
        shards: 1,
        other: {
            rs: {nodes: 2},
            rsOptions:
                {setParameter: "featureFlagAlwaysCreateConfigTransactionsPartialIndexOnStepUp=true"}
        }
    });

    // Sanity check the feature flag was enabled.
    assert(assert
               .commandWorked(featureFlagSt.rs0.getPrimary().adminCommand({
                   getParameter: 1,
                   featureFlagAlwaysCreateConfigTransactionsPartialIndexOnStepUp: 1
               }))
               .featureFlagAlwaysCreateConfigTransactionsPartialIndexOnStepUp.value);
    assert(assert
               .commandWorked(featureFlagSt.rs0.getSecondary().adminCommand({
                   getParameter: 1,
                   featureFlagAlwaysCreateConfigTransactionsPartialIndexOnStepUp: 1
               }))
               .featureFlagAlwaysCreateConfigTransactionsPartialIndexOnStepUp.value);

    runTest(featureFlagSt, true /* alwaysCreateFeatureFlagEnabled */);
    featureFlagSt.stop();
}
})();
