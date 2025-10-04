/**
 * Tests that changing the shard key value of a document using update and findAndModify works
 * correctly when the new shard key value belongs to the same shard.
 * @tags: [
 *   uses_multi_shard_transaction,
 *   uses_transactions,
 * ]
 */

import {withTxnAndAutoRetryOnMongos} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {enableCoordinateCommitReturnImmediatelyAfterPersistingDecision} from "jstests/sharding/libs/sharded_transactions_helpers.js";
import {
    assertCanDoReplacementUpdateWhereShardKeyMissingFields,
    assertCannotUpdate_id,
    assertCannotUpdate_idDottedPath,
    assertCannotUpdateSKToArray,
    assertCannotUpdateWithMultiTrue,
    assertCanUnsetSKField,
    assertCanUpdateDottedPath,
    assertCanUpdateInBulkOpWhenDocsRemainOnSameShard,
    assertCanUpdatePartialShardKey,
    assertCanUpdatePrimitiveShardKey,
    assertCanUpdatePrimitiveShardKeyHashedSameShards,
    shardCollectionMoveChunks,
} from "jstests/sharding/libs/update_shard_key_helpers.js";

const st = new ShardingTest({
    mongos: 1,
    shards: {rs0: {nodes: 3}, rs1: {nodes: 3}},
    rsOptions: {setParameter: {maxTransactionLockRequestTimeoutMillis: ReplSetTest.kDefaultTimeoutMS}},
});
const kDbName = "db";
const ns = kDbName + ".foo";
const mongos = st.s0;
const shard0 = st.shard0.shardName;

enableCoordinateCommitReturnImmediatelyAfterPersistingDecision(st);
assert.commandWorked(mongos.adminCommand({enableSharding: kDbName, primaryShard: shard0}));

// ---------------------------------
// Update shard key retryable write
// ---------------------------------

let session = st.s.startSession({retryWrites: true});
let sessionDB = session.getDatabase(kDbName);

// Modify updates

// upsert : false
assertCanUpdatePrimitiveShardKey(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    false,
    false,
    [{"x": 300}, {"x": 4}],
    [{"$set": {"x": 600}}, {"$set": {"x": 30}}],
    false,
);
assertCanUpdateDottedPath(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    false,
    false,
    [{"x.a": 300}, {"x.a": 4}],
    [{"$set": {"x": {"a": 600}}}, {"$set": {"x": {"a": 30}}}],
    false,
);
assertCanUpdatePartialShardKey(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    false,
    false,
    [
        {"x": 300, "y": 80},
        {"x": 4, "y": 3},
    ],
    [{"$set": {"x": 600}}, {"$set": {"x": 30}}],
    false,
);
assertCanUnsetSKField(st, kDbName, ns, session, sessionDB, false, false, {"x": 300}, {"$unset": {"x": 1}}, false);

// upsert : true
assertCanUpdatePrimitiveShardKey(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    false,
    false,
    [{"x": 900}, {"x": 3}],
    [{"$set": {"x": 600}}, {"$set": {"x": 30}}],
    true,
);
assertCanUpdateDottedPath(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    false,
    false,
    [{"x.a": 300}, {"x.a": 4}],
    [{"$set": {"x": {"a": 600}}}, {"$set": {"x": {"a": 30}}}],
    true,
);
assertCanUpdatePartialShardKey(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    false,
    false,
    [
        {"x": 300, "y": 80},
        {"x": 4, "y": 3},
    ],
    [{"$set": {"x": 600}}, {"$set": {"x": 30}}],
    true,
);
assertCanUnsetSKField(st, kDbName, ns, session, sessionDB, false, false, {"x": 300}, {"$unset": {"x": 1}}, true);

// failing cases
assertCannotUpdate_id(st, kDbName, ns, session, sessionDB, false, false, {"_id": 300}, {"$set": {"_id": 600}});
assertCannotUpdate_idDottedPath(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    false,
    false,
    {"_id.a": 300},
    {
        "$set": {"_id": {"a": 600}},
    },
);
assertCannotUpdateWithMultiTrue(st, kDbName, ns, session, sessionDB, false, {"x": 300}, {"$set": {"x": 600}});
assertCannotUpdateSKToArray(st, kDbName, ns, session, sessionDB, false, false, {"x": 300}, {"$set": {"x": [300]}});

// Replacement updates

// upsert : false
assertCanUpdatePrimitiveShardKey(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    false,
    false,
    [{"x": 300}, {"x": 4}],
    [{"x": 600}, {"x": 30}],
    false,
);
assertCanUpdateDottedPath(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    false,
    false,
    [{"x.a": 300}, {"x.a": 4}],
    [{"x": {"a": 600}}, {"x": {"a": 30}}],
    false,
);
assertCanUpdatePartialShardKey(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    false,
    false,
    [
        {"x": 300, "y": 80},
        {"x": 4, "y": 3},
    ],
    [
        {"x": 600, "y": 80},
        {"x": 30, "y": 3},
    ],
    false,
);
assertCanDoReplacementUpdateWhereShardKeyMissingFields(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    false,
    false,
    {"x": 300, "y": 80},
    {"x": 600},
    false,
);
// Shard key fields are missing in query.
assertCanDoReplacementUpdateWhereShardKeyMissingFields(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    false,
    false,
    {"x": 300},
    {"x": 600, "y": 80, "a": 2},
    false,
);

// upsert : true
assertCanUpdatePrimitiveShardKey(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    false,
    false,
    [{"x": 300}, {"x": 4}],
    [{"x": 600}, {"x": 30}],
    true,
);
assertCanUpdateDottedPath(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    false,
    false,
    [{"x.a": 300}, {"x.a": 4}],
    [{"x": {"a": 600}}, {"x": {"a": 30}}],
    true,
);
assertCanUpdatePartialShardKey(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    false,
    false,
    [
        {"x": 300, "y": 80},
        {"x": 4, "y": 3},
    ],
    [
        {"x": 600, "y": 80},
        {"x": 30, "y": 3},
    ],
    true,
);
assertCanDoReplacementUpdateWhereShardKeyMissingFields(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    false,
    false,
    {"x": 300, "y": 80},
    {"x": 600},
    true,
);
// Shard key fields are missing in query.
assertCanDoReplacementUpdateWhereShardKeyMissingFields(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    false,
    false,
    {"x": 300},
    {"x": 600, "y": 80, "a": 2},
    true,
);

// failing cases
assertCannotUpdate_id(st, kDbName, ns, session, sessionDB, false, false, {"_id": 300}, {"_id": 600});
assertCannotUpdate_idDottedPath(st, kDbName, ns, session, sessionDB, false, false, {"_id.a": 300}, {"_id": {"a": 600}});
assertCannotUpdateWithMultiTrue(st, kDbName, ns, session, sessionDB, false, {"x": 300}, {"x": 600});
assertCannotUpdateSKToArray(st, kDbName, ns, session, sessionDB, false, false, {"x": 300}, {"x": [300]});

// Modify style findAndModify

// upsert : false
assertCanUpdatePrimitiveShardKey(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    false,
    true,
    [{"x": 300}, {"x": 4}],
    [{"$set": {"x": 600}}, {"$set": {"x": 30}}],
    false,
);
assertCanUpdateDottedPath(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    false,
    true,
    [{"x.a": 300}, {"x.a": 4}],
    [{"$set": {"x": {"a": 600}}}, {"$set": {"x": {"a": 30}}}],
    false,
);
assertCanUpdatePartialShardKey(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    false,
    true,
    [
        {"x": 300, "y": 80},
        {"x": 4, "y": 3},
    ],
    [{"$set": {"x": 600}}, {"$set": {"x": 30}}],
    false,
);
assertCanUnsetSKField(st, kDbName, ns, session, sessionDB, false, true, {"x": 300}, {"$unset": {"x": 1}}, false);

// upsert : true
assertCanUpdatePrimitiveShardKey(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    false,
    false,
    [{"x": 300}, {"x": 4}],
    [{"$set": {"x": 600}}, {"$set": {"x": 30}}],
    true,
);
assertCanUpdateDottedPath(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    false,
    false,
    [{"x.a": 300}, {"x.a": 4}],
    [{"$set": {"x": {"a": 600}}}, {"$set": {"x": {"a": 30}}}],
    true,
);
assertCanUpdatePartialShardKey(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    false,
    true,
    [
        {"x": 300, "y": 80},
        {"x": 4, "y": 3},
    ],
    [{"$set": {"x": 600}}, {"$set": {"x": 30}}],
    true,
);
assertCanUnsetSKField(st, kDbName, ns, session, sessionDB, false, true, {"x": 300}, {"$unset": {"x": 1}}, true);

// failing cases
assertCannotUpdate_id(st, kDbName, ns, session, sessionDB, false, true, {"_id": 300}, {"$set": {"_id": 600}});
assertCannotUpdate_idDottedPath(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    false,
    true,
    {"_id.a": 300},
    {
        "$set": {"_id": {"a": 600}},
    },
);
assertCannotUpdateSKToArray(st, kDbName, ns, session, sessionDB, false, true, {"x": 300}, {"$set": {"x": [300]}});

// Replacement style findAndModify

// upsert : false
assertCanUpdatePrimitiveShardKey(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    false,
    true,
    [{"x": 300}, {"x": 4}],
    [{"x": 600}, {"x": 30}],
    false,
);
assertCanUpdateDottedPath(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    false,
    true,
    [{"x.a": 300}, {"x.a": 4}],
    [{"x": {"a": 600}}, {"x": {"a": 30}}],
    false,
);
assertCanUpdatePartialShardKey(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    false,
    true,
    [
        {"x": 300, "y": 80},
        {"x": 4, "y": 3},
    ],
    [
        {"x": 600, "y": 80},
        {"x": 30, "y": 3},
    ],
    false,
);
assertCanDoReplacementUpdateWhereShardKeyMissingFields(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    false,
    true,
    {"x": 300, "y": 80},
    {"x": 600},
    false,
);
// Shard key fields are missing in query.
assertCanDoReplacementUpdateWhereShardKeyMissingFields(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    false,
    true,
    {"x": 300},
    {"x": 600, "y": 80, "a": 2},
    false,
);

// upsert: true
assertCanUpdatePrimitiveShardKey(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    false,
    false,
    [{"x": 300}, {"x": 4}],
    [{"x": 600}, {"x": 30}],
    true,
);
assertCanUpdateDottedPath(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    false,
    false,
    [{"x.a": 300}, {"x.a": 4}],
    [{"x": {"a": 600}}, {"x": {"a": 30}}],
    true,
);
assertCanUpdatePartialShardKey(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    false,
    true,
    [
        {"x": 300, "y": 80},
        {"x": 4, "y": 3},
    ],
    [
        {"x": 600, "y": 80},
        {"x": 30, "y": 3},
    ],
    true,
);
assertCanDoReplacementUpdateWhereShardKeyMissingFields(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    false,
    true,
    {"x": 300, "y": 80},
    {"x": 600},
    true,
);
// Shard key fields are missing in query.
assertCanDoReplacementUpdateWhereShardKeyMissingFields(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    false,
    true,
    {"x": 300},
    {"x": 600, "y": 80, "a": 2},
    true,
);

// failing cases
assertCannotUpdate_id(st, kDbName, ns, session, sessionDB, false, true, {"_id": 300}, {"_id": 600});
assertCannotUpdate_idDottedPath(st, kDbName, ns, session, sessionDB, false, true, {"_id.a": 300}, {"_id": {"a": 600}});
assertCannotUpdateSKToArray(st, kDbName, ns, session, sessionDB, false, true, {"x": 300}, {"x": [300]});

// Bulk writes retryable writes
assertCanUpdateInBulkOpWhenDocsRemainOnSameShard(st, kDbName, ns, session, sessionDB, false, false);
assertCanUpdateInBulkOpWhenDocsRemainOnSameShard(st, kDbName, ns, session, sessionDB, false, true);

// ----Assert correct behavior when collection is hash sharded----

assertCanUpdatePrimitiveShardKeyHashedSameShards(st, kDbName, ns, session, sessionDB, true);

// ---------------------------------------
// Update shard key in multi statement txn
// ---------------------------------------

session = st.s.startSession();
sessionDB = session.getDatabase(kDbName);

// ----Single writes in txn----

// Modify updates

// upsert : false
assertCanUpdatePrimitiveShardKey(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    true,
    false,
    [{"x": 300}, {"x": 4}],
    [{"$set": {"x": 600}}, {"$set": {"x": 30}}],
    false,
);
assertCanUpdateDottedPath(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    true,
    false,
    [{"x.a": 300}, {"x.a": 4}],
    [{"$set": {"x": {"a": 600}}}, {"$set": {"x": {"a": 30}}}],
    false,
);
assertCanUpdatePartialShardKey(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    true,
    false,
    [
        {"x": 300, "y": 80},
        {"x": 4, "y": 3},
    ],
    [{"$set": {"x": 600}}, {"$set": {"x": 30}}],
    false,
);
assertCanUnsetSKField(st, kDbName, ns, session, sessionDB, true, false, {"x": 300}, {"$unset": {"x": 1}}, false);

// upsert : true
assertCanUpdatePrimitiveShardKey(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    true,
    false,
    [{"x": 300}, {"x": 4}],
    [{"$set": {"x": 600}}, {"$set": {"x": 30}}],
    true,
);
assertCanUpdateDottedPath(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    true,
    false,
    [{"x.a": 300}, {"x.a": 4}],
    [{"$set": {"x": {"a": 600}}}, {"$set": {"x": {"a": 30}}}],
    true,
);
assertCanUpdatePartialShardKey(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    true,
    false,
    [
        {"x": 300, "y": 80},
        {"x": 4, "y": 3},
    ],
    [{"$set": {"x": 600}}, {"$set": {"x": 30}}],
    true,
);
assertCanUnsetSKField(st, kDbName, ns, session, sessionDB, true, false, {"x": 300}, {"$unset": {"x": 1}}, true);

// failing cases
assertCannotUpdate_id(st, kDbName, ns, session, sessionDB, true, false, {"_id": 300}, {"$set": {"_id": 600}});
assertCannotUpdate_idDottedPath(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    true,
    false,
    {"_id.a": 300},
    {
        "$set": {"_id": {"a": 600}},
    },
);
assertCannotUpdateWithMultiTrue(st, kDbName, ns, session, sessionDB, true, {"x": 300}, {"$set": {"x": 600}});
assertCannotUpdateSKToArray(st, kDbName, ns, session, sessionDB, true, false, {"x": 300}, {"$set": {"x": [300]}});

// Replacement updates

// upsert : false
assertCanUpdatePrimitiveShardKey(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    true,
    false,
    [{"x": 300}, {"x": 4}],
    [{"x": 600}, {"x": 30}],
    false,
);
assertCanUpdateDottedPath(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    true,
    false,
    [{"x.a": 300}, {"x.a": 4}],
    [{"x": {"a": 600}}, {"x": {"a": 30}}],
    false,
);
assertCanUpdatePartialShardKey(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    true,
    false,
    [
        {"x": 300, "y": 80},
        {"x": 4, "y": 3},
    ],
    [
        {"x": 600, "y": 80},
        {"x": 30, "y": 3},
    ],
    false,
);
assertCanDoReplacementUpdateWhereShardKeyMissingFields(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    true,
    false,
    {"x": 300, "y": 80},
    {"x": 600},
    false,
);
// Shard key fields are missing in query.
assertCanDoReplacementUpdateWhereShardKeyMissingFields(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    true,
    false,
    {"x": 300},
    {"x": 600, "y": 80, "a": 2},
    false,
);

// upsert : true
assertCanUpdatePrimitiveShardKey(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    true,
    false,
    [{"x": 300}, {"x": 4}],
    [{"x": 600}, {"x": 30}],
    true,
);
assertCanUpdateDottedPath(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    true,
    false,
    [{"x.a": 300}, {"x.a": 4}],
    [{"x": {"a": 600}}, {"x": {"a": 30}}],
    true,
);
assertCanUpdatePartialShardKey(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    true,
    false,
    [
        {"x": 300, "y": 80},
        {"x": 4, "y": 3},
    ],
    [
        {"x": 600, "y": 80},
        {"x": 30, "y": 3},
    ],
    true,
);
assertCanDoReplacementUpdateWhereShardKeyMissingFields(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    true,
    false,
    {"x": 300, "y": 80},
    {"x": 600},
    true,
);
// Shard key fields are missing in query.
assertCanDoReplacementUpdateWhereShardKeyMissingFields(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    true,
    false,
    {"x": 300},
    {"x": 600, "y": 80, "a": 2},
    true,
);

// failing cases
assertCannotUpdate_id(st, kDbName, ns, session, sessionDB, true, false, {"_id": 300}, {"_id": 600});
assertCannotUpdate_idDottedPath(st, kDbName, ns, session, sessionDB, true, false, {"_id.a": 300}, {"_id": {"a": 600}});
assertCannotUpdateWithMultiTrue(st, kDbName, ns, session, sessionDB, true, {"x": 300}, {"x": 600});
assertCannotUpdateSKToArray(st, kDbName, ns, session, sessionDB, true, false, {"x": 300}, {"x": [300]});

// Modify style findAndModify

// upsert : false
assertCanUpdatePrimitiveShardKey(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    true,
    true,
    [{"x": 300}, {"x": 4}],
    [{"$set": {"x": 600}}, {"$set": {"x": 30}}],
    false,
);
assertCanUpdateDottedPath(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    true,
    true,
    [{"x.a": 300}, {"x.a": 4}],
    [{"$set": {"x": {"a": 600}}}, {"$set": {"x": {"a": 30}}}],
    false,
);
assertCanUpdatePartialShardKey(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    true,
    true,
    [
        {"x": 300, "y": 80},
        {"x": 4, "y": 3},
    ],
    [{"$set": {"x": 600}}, {"$set": {"x": 30}}],
    false,
);
assertCanUnsetSKField(st, kDbName, ns, session, sessionDB, true, true, {"x": 300}, {"$unset": {"x": 1}}, false);

// upsert : true
assertCanUpdatePrimitiveShardKey(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    true,
    true,
    [{"x": 300}, {"x": 4}],
    [{"$set": {"x": 600}}, {"$set": {"x": 30}}],
    true,
);
assertCanUpdateDottedPath(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    true,
    true,
    [{"x.a": 300}, {"x.a": 4}],
    [{"$set": {"x": {"a": 600}}}, {"$set": {"x": {"a": 30}}}],
    true,
);
assertCanUpdatePartialShardKey(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    true,
    true,
    [
        {"x": 300, "y": 80},
        {"x": 4, "y": 3},
    ],
    [{"$set": {"x": 600}}, {"$set": {"x": 30}}],
    true,
);
assertCanUnsetSKField(st, kDbName, ns, session, sessionDB, true, true, {"x": 300}, {"$unset": {"x": 1}}, true);

// failing cases
assertCannotUpdate_id(st, kDbName, ns, session, sessionDB, true, true, {"_id": 300}, {"$set": {"_id": 600}});
assertCannotUpdate_idDottedPath(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    true,
    true,
    {"_id.a": 300},
    {"$set": {"_id": {"a": 600}}},
);
assertCannotUpdateSKToArray(st, kDbName, ns, session, sessionDB, true, true, {"x": 300}, {"$set": {"x": [300]}});

// Replacement style findAndModify

// upsert : false
assertCanUpdatePrimitiveShardKey(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    true,
    true,
    [{"x": 300}, {"x": 4}],
    [{"x": 600}, {"x": 30}],
    false,
);
assertCanUpdateDottedPath(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    true,
    true,
    [{"x.a": 300}, {"x.a": 4}],
    [{"x": {"a": 600}}, {"x": {"a": 30}}],
    false,
);
assertCanUpdatePartialShardKey(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    true,
    true,
    [
        {"x": 300, "y": 80},
        {"x": 4, "y": 3},
    ],
    [
        {"x": 600, "y": 80},
        {"x": 30, "y": 3},
    ],
    false,
);
assertCanDoReplacementUpdateWhereShardKeyMissingFields(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    true,
    true,
    {"x": 300, "y": 80},
    {"x": 600},
    false,
);
// Shard key fields are missing in query.
assertCanDoReplacementUpdateWhereShardKeyMissingFields(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    true,
    true,
    {"x": 300},
    {"x": 600, "y": 80, "a": 2},
    false,
);

// upsert : true
assertCanUpdatePrimitiveShardKey(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    true,
    true,
    [{"x": 300}, {"x": 4}],
    [{"x": 600}, {"x": 30}],
    true,
);
assertCanUpdateDottedPath(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    true,
    true,
    [{"x.a": 300}, {"x.a": 4}],
    [{"x": {"a": 600}}, {"x": {"a": 30}}],
    true,
);
assertCanUpdatePartialShardKey(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    true,
    true,
    [
        {"x": 300, "y": 80},
        {"x": 4, "y": 3},
    ],
    [
        {"x": 600, "y": 80},
        {"x": 30, "y": 3},
    ],
    true,
);
assertCanDoReplacementUpdateWhereShardKeyMissingFields(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    true,
    true,
    {"x": 300, "y": 80},
    {"x": 600},
    true,
);
// Shard key fields are missing in query.
assertCanDoReplacementUpdateWhereShardKeyMissingFields(
    st,
    kDbName,
    ns,
    session,
    sessionDB,
    true,
    true,
    {"x": 300},
    {"x": 600, "y": 80, "a": 2},
    true,
);

// failing cases
assertCannotUpdate_id(st, kDbName, ns, session, sessionDB, true, true, {"_id": 300}, {"_id": 600});
assertCannotUpdate_idDottedPath(st, kDbName, ns, session, sessionDB, true, true, {"_id.a": 300}, {"_id": {"a": 600}});
assertCannotUpdateSKToArray(st, kDbName, ns, session, sessionDB, true, true, {"x": 300}, {"x": [300]});

// ----Assert correct behavior when collection is hash sharded----

assertCanUpdatePrimitiveShardKeyHashedSameShards(st, kDbName, ns, session, sessionDB, true);

// ----Multiple writes in txn-----

// Bulk writes in txn
assertCanUpdateInBulkOpWhenDocsRemainOnSameShard(st, kDbName, ns, session, sessionDB, true, false);
assertCanUpdateInBulkOpWhenDocsRemainOnSameShard(st, kDbName, ns, session, sessionDB, true, true);

// Update two docs, updating one twice
let docsToInsert = [{"x": 4, "a": 3}, {"x": 100}, {"x": 300, "a": 3}, {"x": 500, "a": 6}];
shardCollectionMoveChunks(st, kDbName, ns, {"x": 1}, docsToInsert, {"x": 100}, {"x": 300});

let id;
withTxnAndAutoRetryOnMongos(session, () => {
    id = mongos.getDB(kDbName).foo.find({"x": 500}).toArray()[0]._id;
    assert.commandWorked(sessionDB.foo.update({"x": 500}, {"$set": {"x": 400}}));
    assert.commandWorked(sessionDB.foo.update({"x": 400}, {"x": 600, "_id": id}));
    assert.commandWorked(sessionDB.foo.update({"x": 4}, {"$set": {"x": 30}}));
});

assert.eq(0, sessionDB.foo.find({"x": 500}).itcount());
assert.eq(0, sessionDB.foo.find({"x": 400}).itcount());
assert.eq(1, sessionDB.foo.find({"x": 600}).itcount());
assert.eq(id, sessionDB.foo.find({"x": 600}).toArray()[0]._id);
assert.eq(0, sessionDB.foo.find({"x": 4}).itcount());
assert.eq(1, sessionDB.foo.find({"x": 30}).itcount());

mongos.getDB(kDbName).foo.drop();

// Check that doing $inc on doc A, then updating shard key for doc A, then $inc again only incs
// once
shardCollectionMoveChunks(st, kDbName, ns, {"x": 1}, docsToInsert, {"x": 100}, {"x": 300});

withTxnAndAutoRetryOnMongos(session, () => {
    assert.commandWorked(sessionDB.foo.update({"x": 500}, {"$inc": {"a": 1}}));
    assert.commandWorked(sessionDB.foo.update({"x": 500}, {"$set": {"x": 400}}));
    assert.commandWorked(sessionDB.foo.update({"x": 500}, {"$inc": {"a": 1}}));
});

assert.eq(0, sessionDB.foo.find({"x": 500}).itcount());
assert.eq(1, sessionDB.foo.find({"x": 400}).itcount());
assert.eq(1, sessionDB.foo.find({"a": 7}).itcount());
assert.eq(1, sessionDB.foo.find({"x": 400, "a": 7}).itcount());

mongos.getDB(kDbName).foo.drop();

// Check that doing findAndModify to update shard key followed by $inc works correctly
shardCollectionMoveChunks(st, kDbName, ns, {"x": 1}, docsToInsert, {"x": 100}, {"x": 300});

withTxnAndAutoRetryOnMongos(session, () => {
    sessionDB.foo.findAndModify({query: {"x": 500}, update: {$set: {"x": 600}}});
    assert.commandWorked(sessionDB.foo.update({"x": 600}, {"$inc": {"a": 1}}));
});

assert.eq(0, sessionDB.foo.find({"x": 500}).itcount());
assert.eq(1, sessionDB.foo.find({"x": 600}).itcount());
assert.eq(1, sessionDB.foo.find({"a": 7}).itcount());
assert.eq(1, sessionDB.foo.find({"x": 600, "a": 7}).itcount());

mongos.getDB(kDbName).foo.drop();

// Check that doing findAndModify followed by and update on a shard key works correctly
shardCollectionMoveChunks(st, kDbName, ns, {"x": 1}, docsToInsert, {"x": 100}, {"x": 300});

id = mongos.getDB(kDbName).foo.find({"x": 4}).toArray()[0]._id;
withTxnAndAutoRetryOnMongos(session, () => {
    sessionDB.foo.findAndModify({query: {"x": 4}, update: {$set: {"x": 20}}});
    assert.commandWorked(sessionDB.foo.update({"x": 20}, {$set: {"x": 1}}));
});

assert.eq(0, sessionDB.foo.find({"x": 4}).itcount());
assert.eq(0, sessionDB.foo.find({"x": 20}).itcount());
assert.eq(1, sessionDB.foo.find({"x": 1}).itcount());
assert.eq(id, sessionDB.foo.find({"x": 1}).toArray()[0]._id);

mongos.getDB(kDbName).foo.drop();

st.stop();
