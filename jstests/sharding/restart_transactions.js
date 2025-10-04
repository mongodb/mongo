/**
 * Verify the states that a multi-statement transaction can be restarted on at the active
 * transaction number for servers in a sharded cluster.
 *
 * @tags: [requires_sharding, uses_transactions, uses_prepare_transaction]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

// This test requires running transactions directly against the shard.
TestData.replicaSetEndpointIncompatible = true;

const collName = "restart_transactions";

function runTest(routerDB, directDB) {
    // Set up the underlying collection.
    const routerColl = routerDB[collName];
    assert.commandWorked(routerDB.createCollection(routerColl.getName(), {writeConcern: {w: "majority"}}));

    // Read from the mongoS to ensure the shard has refreshed its filtering information.
    assert.eq(0, routerDB.getCollection(collName).countDocuments({}));

    //
    // Can restart a transaction that has been aborted.
    //

    let txnNumber = 0;
    assert.commandWorked(
        directDB.runCommand({
            find: collName,
            txnNumber: NumberLong(txnNumber),
            autocommit: false,
            startTransaction: true,
        }),
    );
    assert.commandWorked(
        directDB.adminCommand({abortTransaction: 1, txnNumber: NumberLong(txnNumber), autocommit: false}),
    );

    assert.commandWorked(
        directDB.runCommand({
            find: collName,
            txnNumber: NumberLong(txnNumber),
            autocommit: false,
            startTransaction: true,
        }),
    );

    //
    // Cannot restart a transaction that is in progress.
    //

    txnNumber++;
    assert.commandWorked(
        directDB.runCommand({
            find: collName,
            txnNumber: NumberLong(txnNumber),
            autocommit: false,
            startTransaction: true,
        }),
    );

    assert.commandFailedWithCode(
        directDB.runCommand({
            find: collName,
            txnNumber: NumberLong(txnNumber),
            autocommit: false,
            startTransaction: true,
        }),
        50911,
    );

    //
    // Cannot restart a transaction that has completed a retryable write.
    //

    txnNumber++;
    assert.commandWorked(
        directDB.runCommand({insert: collName, documents: [{x: txnNumber}], txnNumber: NumberLong(txnNumber)}),
    );

    assert.commandFailedWithCode(
        directDB.runCommand({
            find: collName,
            txnNumber: NumberLong(txnNumber),
            autocommit: false,
            startTransaction: true,
        }),
        50911,
    );

    //
    // Cannot restart a transaction that has been committed.
    //

    txnNumber++;
    assert.commandWorked(
        directDB.runCommand({
            find: collName,
            txnNumber: NumberLong(txnNumber),
            autocommit: false,
            startTransaction: true,
        }),
    );
    assert.commandWorked(
        directDB.adminCommand({
            commitTransaction: 1,
            txnNumber: NumberLong(txnNumber),
            autocommit: false,
            writeConcern: {w: "majority"},
        }),
    );

    assert.commandFailedWithCode(
        directDB.runCommand({
            find: collName,
            txnNumber: NumberLong(txnNumber),
            autocommit: false,
            startTransaction: true,
        }),
        50911,
    );

    //
    // Cannot restart a transaction that has been prepared.
    //

    txnNumber++;
    assert.commandWorked(
        directDB.runCommand({
            find: collName,
            txnNumber: NumberLong(txnNumber),
            autocommit: false,
            startTransaction: true,
        }),
    );
    assert.commandWorked(
        directDB.adminCommand({prepareTransaction: 1, txnNumber: NumberLong(txnNumber), autocommit: false}),
    );

    assert.commandFailedWithCode(
        directDB.runCommand({
            find: collName,
            txnNumber: NumberLong(txnNumber),
            autocommit: false,
            startTransaction: true,
        }),
        50911,
    );

    assert.commandWorked(
        directDB.adminCommand({abortTransaction: 1, txnNumber: NumberLong(txnNumber), autocommit: false}),
    );

    //
    // Cannot restart a transaction that has been aborted after being prepared.
    //

    txnNumber++;
    assert.commandWorked(
        directDB.runCommand({
            find: collName,
            txnNumber: NumberLong(txnNumber),
            autocommit: false,
            startTransaction: true,
        }),
    );
    assert.commandWorked(
        directDB.adminCommand({prepareTransaction: 1, txnNumber: NumberLong(txnNumber), autocommit: false}),
    );
    assert.commandWorked(
        directDB.adminCommand({abortTransaction: 1, txnNumber: NumberLong(txnNumber), autocommit: false}),
    );

    assert.commandFailedWithCode(
        directDB.runCommand({
            find: collName,
            txnNumber: NumberLong(txnNumber),
            autocommit: false,
            startTransaction: true,
        }),
        50911,
    );
}

const st = new ShardingTest({shards: 1, mongos: 1});

// Directly connect to the shard primary to simulate internal retries by mongos.
const shardDBName = "test";
const shardSession = st.rs0.getPrimary().startSession({causalConsistency: false});
const shardDB = shardSession.getDatabase(shardDBName);

runTest(st.s.getDB(shardDBName), shardDB);

// Directly connect to the config sever primary to simulate internal retries by mongos.
const configDBName = "config";
const configSession = st.configRS.getPrimary().startSession({causalConsistency: false});
const configDB = configSession.getDatabase(configDBName);

runTest(st.s.getDB(configDBName), configDB);

st.stop();
