/**
 * Tests that:
 * 1. Reads and writes to the admin database are forbidden within single replica set transactions
 *    on sharded clusters.
 * 2. Read and writes to the config database are forbidden from mongos within single replica set
 *    transactions on sharded clusters.
 * 3. Reads and writes to the config.transactions namespace are forbidden within single replica set
 *    transactions on sharded clusters, BUT read and writes to other namespaces in the config
 *    database are allowed.
 *
 * @tags: [requires_find_command]
 */

(function() {
"use strict";

load("jstests/sharding/libs/sharded_transactions_helpers.js");

const st = new ShardingTest({shards: 1});
const mongosSession = st.s.startSession();
const shardSession = st.shard0.getDB("test").getMongo().startSession();
const collName = "banned_txn_dbs";

jsTest.log("Verify that read and write operations within transactions are forbidden for the " +
           "admin database within a transaction.");

const adminColl = shardSession.getDatabase("admin")[collName];

shardSession.startTransaction();
let error = assert.throws(() => adminColl.find().itcount());
assert.commandFailedWithCode(error, ErrorCodes.OperationNotSupportedInTransaction);
assert.commandFailedWithCode(shardSession.abortTransaction_forTesting(),
                             ErrorCodes.NoSuchTransaction);

shardSession.startTransaction();
assert.commandFailedWithCode(adminColl.insert({}), ErrorCodes.OperationNotSupportedInTransaction);
assert.commandFailedWithCode(shardSession.abortTransaction_forTesting(),
                             ErrorCodes.NoSuchTransaction);

jsTestLog("Verify that read and write operations within transactions are forbidden for the " +
          "config database when accessed through mongos.");

const mongosConfigDB = mongosSession.getDatabase("config");
const clusterColls = [
    mongosConfigDB["test"],
    mongosConfigDB["actionlog"],
    mongosConfigDB["transaction_coords"],
    mongosConfigDB["transactions"]
];

mongosSession.startTransaction();
clusterColls.forEach((coll) => {
    error = assert.throws(() => coll.find().itcount());
    assert.commandFailedWithCode(error, ErrorCodes.OperationNotSupportedInTransaction);
});

mongosSession.endSession();

jsTestLog("Verify that read operations within transactions work fine for the config database " +
          "when not config.transactions (and directly accessed through the shard).");

const configDB = shardSession.getDatabase("config");
const shardColls = [configDB["test"], configDB["actionlog"], configDB["transaction_coords"]];

shardSession.startTransaction();
shardColls.forEach((coll) => {
    coll.find().itcount();
});

jsTestLog("Verify that read operations will not work for the config.transactions namespace.");

const shardCollTransactions = configDB["transactions"];
error = assert.throws(() => shardCollTransactions.find().itcount());
assert.commandFailedWithCode(error, ErrorCodes.OperationNotSupportedInTransaction);

shardSession.endSession();
st.stop();
}());
