/*
 * Tests that operations that run on a shard acting in the shard role, and during their execution
 * transition into a router role that immediately reenters into the shard role locally (without
 * going over a network command) are able to deal with the shard needing to recover its filtering
 * metadata.
 */

import {withRetryOnTransientTxnError} from "jstests/libs/auto_retry_transaction_in_sharding.js";

const st = new ShardingTest({shards: 2, rs: {nodes: 3}});

const dbName = "test";
const collName1 = "my_coll_1";
const collName2 = "my_coll_2";
const viewName = "my_view";

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard1.shardName}));

const db = st.s.getDB(dbName);
const coll1 = db[collName1];
const coll2 = db[collName2];

assert.commandWorked(coll1.insert({_id: 1, x: 1}));
assert.commandWorked(coll2.insert({_id: 1, x: 1}));

db.createView(viewName, collName1, [{$group: {_id: null, xlField: {$addToSet: "$x"}}}]);

// Step up a secondary shard that does not have filtering metadata currently installed.
st.rs0.freeze(st.rs0.getPrimary());
st.rs1.freeze(st.rs1.getPrimary());

let session = st.s.startSession();

// Read from a view within a transaction.
withRetryOnTransientTxnError(
    () => {
        session.startTransaction();
        let sessionDB = session.getDatabase(dbName);

        assert.eq(sessionDB[viewName].find().itcount(), 1);
        assert.commandWorked(session.commitTransaction_forTesting());
    },
    () => {
        session.abortTransaction_forTesting();
    });

// Run an aggregation that includes $lookup to a second collection within a transaction.
withRetryOnTransientTxnError(
    () => {
        session.startTransaction();
        let sessionDB = session.getDatabase(dbName);

        assert.eq(
            sessionDB[collName1]
                .aggregate(
                    [{$lookup: {from: collName2, localField: 'x', foreignField: 'x', as: 'j'}}])
                .itcount(),
            1);
        assert.commandWorked(session.commitTransaction_forTesting());
    },
    () => {
        session.abortTransaction_forTesting();
    });

st.stop();
