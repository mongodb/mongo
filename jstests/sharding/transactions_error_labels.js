// Test TransientTransactionErrors error label in mongos write commands.
// @tags: [uses_transactions, uses_multi_shard_transaction]
(function() {
    "use strict";

    load("jstests/sharding/libs/sharded_transactions_helpers.js");

    const dbName = "test";
    const collName = "foo";
    const ns = dbName + "." + collName;

    const failCommandWithError = function(rst, commandToFail, errorCode) {
        rst.nodes.forEach(function(node) {
            assert.commandWorked(node.getDB("admin").runCommand({
                configureFailPoint: "failCommand",
                mode: "alwaysOn",
                data: {
                    errorCode: errorCode,
                    failCommands: [commandToFail],
                    failInternalCommands: true  // mongod sees mongos as an internal client
                }
            }));
        });
    };

    const failCommandWithWriteConcernError = function(rst, commandToFail) {
        rst.nodes.forEach(function(node) {
            assert.commandWorked(node.getDB("admin").runCommand({
                configureFailPoint: "failCommand",
                mode: "alwaysOn",
                data: {
                    writeConcernError: {code: NumberInt(12345), errmsg: "dummy"},
                    failCommands: [commandToFail],
                    failInternalCommands: true  // mongod sees mongos as an internal client
                }
            }));
        });
    };

    const turnOffFailCommand = function(rst) {
        rst.nodes.forEach(function(node) {
            assert.commandWorked(
                node.getDB("admin").runCommand({configureFailPoint: "failCommand", mode: "off"}));
        });
    };

    let numCalls = 0;
    const startTransaction = function(mongosSession, dbName, collName) {
        numCalls++;
        mongosSession.startTransaction();
        return mongosSession.getDatabase(dbName).runCommand({
            insert: collName,
            // Target both chunks, wherever they may be
            documents: [{_id: -1 * numCalls}, {_id: numCalls}],
            readConcern: {level: "snapshot"},
        });
    };

    const abortTransactionDirectlyOnParticipant = function(rst, lsid, txnNumber) {
        assert.commandWorked(rst.getPrimary().adminCommand({
            abortTransaction: 1,
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            autocommit: false,
        }));
    };

    const commitTransaction = function(mongosSession) {
        let res = mongosSession.commitTransaction_forTesting();
        print("commitTransaction response from mongos: " + tojson(res));
        return res;
    };

    const checkMongosResponse = function(
        res, expectedErrorCode, expectedErrorLabel, writeConcernErrorExpected) {
        if (expectedErrorCode) {
            assert.eq(0, res.ok, tojson(res));
            assert.eq(expectedErrorCode, res.code, tojson(res));
        } else {
            assert.eq(1, res.ok, tojson(res));
        }

        if (expectedErrorLabel) {
            assert.neq(null, res.errorLabels, tojson(res));
            assert.contains(expectedErrorLabel, res.errorLabels, tojson(res));
        } else {
            assert.eq(null, res.errorLabels, tojson(res));
        }

        if (writeConcernErrorExpected) {
            assert.neq(null, res.writeConcernError, tojson(res));
        } else {
            assert.eq(null, res.writeConcernError, tojson(res));
        }
    };

    const runCommitTests = function(commandSentToShard) {
        jsTest.log("Mongos does not attach any error label if " + commandSentToShard +
                   " returns success.");
        assert.commandWorked(startTransaction(mongosSession, dbName, collName));
        res = mongosSession.commitTransaction_forTesting();
        checkMongosResponse(res, null, null, null);

        jsTest.log("Mongos does not attach any error label if " + commandSentToShard +
                   " returns success with writeConcern error.");
        failCommandWithWriteConcernError(st.rs0, commandSentToShard);
        assert.commandWorked(startTransaction(mongosSession, dbName, collName));
        res = mongosSession.commitTransaction_forTesting();
        checkMongosResponse(res, null, null, true);
        turnOffFailCommand(st.rs0);

        jsTest.log("Mongos attaches 'TransientTransactionError' label if " + commandSentToShard +
                   " returns NoSuchTransaction.");
        assert.commandWorked(startTransaction(mongosSession, dbName, collName));
        abortTransactionDirectlyOnParticipant(
            st.rs0, mongosSession.getSessionId(), mongosSession.getTxnNumber_forTesting());
        res = mongosSession.commitTransaction_forTesting();
        checkMongosResponse(res, ErrorCodes.NoSuchTransaction, "TransientTransactionError", null);
        turnOffFailCommand(st.rs0);

        jsTest.log("Mongos does not attach any error label if " + commandSentToShard +
                   " returns NoSuchTransaction with writeConcern error.");
        failCommandWithWriteConcernError(st.rs0, commandSentToShard);
        assert.commandWorked(startTransaction(mongosSession, dbName, collName));
        abortTransactionDirectlyOnParticipant(
            st.rs0, mongosSession.getSessionId(), mongosSession.getTxnNumber_forTesting());
        res = mongosSession.commitTransaction_forTesting();
        checkMongosResponse(res, ErrorCodes.NoSuchTransaction, null, true);
        turnOffFailCommand(st.rs0);
    };

    let st = new ShardingTest({shards: 2, config: 1, mongosOptions: {verbose: 3}});

    // Create a sharded collection with a chunk on each shard:
    // shard0: [-inf, 0)
    // shard1: [0, +inf)
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));

    // These forced refreshes are not strictly necessary; they just prevent extra TXN log lines
    // from the shards starting, aborting, and restarting the transaction due to needing to
    // refresh after the transaction has started.
    assert.commandWorked(st.shard0.adminCommand({_flushRoutingTableCacheUpdates: ns}));
    assert.commandWorked(st.shard1.adminCommand({_flushRoutingTableCacheUpdates: ns}));

    let mongosSession = st.s.startSession();
    let mongosSessionDB = mongosSession.getDatabase(dbName);

    let res;

    // write statement
    jsTest.log(
        "'TransientTransactionError' label is attached if write statement returns WriteConflict");
    failCommandWithError(st.rs0, "insert", ErrorCodes.WriteConflict);
    res = startTransaction(mongosSession, dbName, collName);
    checkMongosResponse(res, ErrorCodes.WriteConflict, "TransientTransactionError", null);
    turnOffFailCommand(st.rs0);
    mongosSession.abortTransaction();

    // commitTransaction for single-shard transaction (mongos sends commitTransaction)
    runCommitTests("commitTransaction");

    // commitTransaction for multi-shard transaction (mongos sends coordinateCommitTransaction)
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard1.shardName}));
    flushRoutersAndRefreshShardMetadata(st, {ns});
    runCommitTests("coordinateCommitTransaction");

    st.stop();
}());
