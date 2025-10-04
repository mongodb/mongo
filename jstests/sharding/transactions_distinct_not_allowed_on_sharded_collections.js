/**
 * Verifies running distinct in a multi document transaction on a sharded collection is not allowed.
 * This is because distinct does not filter orphaned documents.
 *
 * @tags: [uses_transactions]
 */
import {
    withAbortAndRetryOnTransientTxnError,
    withTxnAndAutoRetryOnMongos,
} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {flushRoutersAndRefreshShardMetadata} from "jstests/sharding/libs/sharded_transactions_helpers.js";

const st = new ShardingTest({shards: 1});

// Set up a sharded and unsharded collection, each with one document.

const unshardedDbName = "unsharded_db";
const unshardedCollName = "unsharded_coll";

const shardedDbName = "sharded_db";
const shardedCollName = "sharded_coll";
const shardedNs = shardedDbName + "." + shardedCollName;

assert.commandWorked(st.s.adminCommand({enableSharding: shardedDbName}));
assert.commandWorked(st.s.adminCommand({shardCollection: shardedNs, key: {_id: 1}}));

const session = st.s.startSession();
const unshardedCollDB = session.getDatabase(unshardedDbName);
const shardedCollDB = session.getDatabase(shardedDbName);

assert.commandWorked(unshardedCollDB[unshardedCollName].insert({_id: "jack"}));
assert.commandWorked(shardedCollDB[shardedCollName].insert({_id: "jack"}));

// Reload metadata to avoid stale config or stale database version errors.
flushRoutersAndRefreshShardMetadata(st, {ns: shardedNs, dbNames: [unshardedDbName]});

// Can run distinct on an unsharded collection.
withTxnAndAutoRetryOnMongos(session, () => {
    assert.eq(unshardedCollDB.runCommand({distinct: unshardedCollName, key: "_id"}).values, ["jack"]);
});

// Cannot run distinct on a sharded collection.
withAbortAndRetryOnTransientTxnError(session, () => {
    session.startTransaction();
    assert.commandFailedWithCode(
        shardedCollDB.runCommand({distinct: shardedCollName, key: "_id"}),
        ErrorCodes.OperationNotSupportedInTransaction,
    );
    assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);
});

session.endSession();
st.stop();
