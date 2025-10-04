/**
 * Test to confirm that mongoS's special handling of replacement updates with an exact query on _id
 * behaves as expected in the case where a collection's shard key includes _id:
 *
 * - For update replacements, mongoS combines the _id from the query with the replacement document
 * to target the query towards a single shard, rather than scattering to all shards.
 * - For upsert replacements, which always require an exact shard key match, mongoS combines the _id
 * from the query with the replacement document to produce a complete shard key.
 *
 * These special cases are allowed because mongoD always propagates the _id of an existing document
 * into its replacement, and in the case of an upsert will use the value of _id from the query
 * filter.
 *
 * @tags: [
 *   uses_multi_shard_transactions,
 *   uses_transactions,
 * ]
 */
import {profilerHasSingleMatchingEntryOrThrow, profilerHasZeroMatchingEntriesOrThrow} from "jstests/libs/profiler.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Test deliberately inserts orphans outside of migrations.
TestData.skipCheckOrphans = true;

const st = new ShardingTest({shards: 2, mongos: 1, other: {enableBalancer: false}});

const mongosDB = st.s0.getDB(jsTestName());
const mongosColl = mongosDB.test;

const shard0DB = st.shard0.getDB(jsTestName());
const shard1DB = st.shard1.getDB(jsTestName());

assert.commandWorked(mongosDB.dropDatabase());

// Enable sharding on the test DB and ensure its primary is shard0.
assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName(), primaryShard: st.shard0.shardName}));

// Enables profiling on both shards so that we can verify the targeting behaviour.
function restartProfiling() {
    for (let shardDB of [shard0DB, shard1DB]) {
        shardDB.setProfilingLevel(0);
        shardDB.system.profile.drop();
        shardDB.setProfilingLevel(2);
    }
}

function setUpData() {
    // Write a single document to shard0 and verify that it is present.
    mongosColl.insert({_id: -100, a: -100, msg: "not_updated"});
    assert.docEq([{_id: -100, a: -100, msg: "not_updated"}], shard0DB.test.find({_id: -100}).toArray());

    // Write a document with the same key directly to shard1. This simulates an orphaned
    // document, or the duplicate document which temporarily exists during a chunk migration.
    assert.commandWorked(shard1DB.test.insert({_id: -100, a: -100, msg: "not_updated"}));
    assert.docEq([{_id: -100, a: -100, msg: "not_updated"}], shard1DB.test.find({_id: -100}).toArray());

    // Clear and restart the profiler on both shards.
    restartProfiling();
}

function runReplacementUpdateTestsForHashedShardKey() {
    // Make sure the chunk containing key {_id: -100} is on shard0; the chunk is migrated twice to
    // ensure that the shard CSR has also a not-UNKOWN version. This will allow direct writes
    // performed later in this test to work because the filtering metadata are set.
    assert.commandWorked(
        mongosDB.adminCommand({
            moveChunk: mongosColl.getFullName(),
            find: {_id: -100},
            to: st.shard1.shardName,
            _waitForDelete: true,
        }),
    );
    assert.commandWorked(
        mongosDB.adminCommand({
            moveChunk: mongosColl.getFullName(),
            find: {_id: -100},
            to: st.shard0.shardName,
            _waitForDelete: true,
        }),
    );

    // Make sure the chunk containing key {_id: 101} is on shard1; the chunk is migrated twice to
    // ensure that the shard CSR has also a not-UNKOWN version. This will allow direct writes
    // performed later in this test to work because the filtering metadata are set.
    assert.commandWorked(
        mongosDB.adminCommand({
            moveChunk: mongosColl.getFullName(),
            find: {_id: 101},
            to: st.shard0.shardName,
            _waitForDelete: true,
        }),
    );
    assert.commandWorked(
        mongosDB.adminCommand({
            moveChunk: mongosColl.getFullName(),
            find: {_id: 101},
            to: st.shard1.shardName,
            _waitForDelete: true,
        }),
    );

    setUpData();

    // Perform a replacement update whose query is an exact match on _id and whose replacement
    // document contains the remainder of the shard key. Despite the fact that the replacement
    // document does not contain the entire shard key, we expect that mongoS will extract the
    // _id from the query and combine it with the replacement doc to target a single shard.
    let writeRes = assert.commandWorked(
        mongosColl.update({_id: -100}, {a: -100, msg: "update_extracted_id_from_query"}),
    );

    // Verify that the update did not modify the orphan document.
    assert.docEq([{_id: -100, a: -100, msg: "not_updated"}], shard1DB.test.find({_id: -100}).toArray());
    assert.eq(writeRes.nMatched, 1);
    assert.eq(writeRes.nModified, 1);

    // Verify that the update only targeted shard0 and that the resulting document appears as
    // expected.
    assert.docEq([{_id: -100, a: -100, msg: "update_extracted_id_from_query"}], mongosColl.find({_id: -100}).toArray());
    profilerHasSingleMatchingEntryOrThrow({
        profileDB: shard0DB,
        filter: {op: "update", "command.u.msg": "update_extracted_id_from_query"},
    });
    profilerHasZeroMatchingEntriesOrThrow({
        profileDB: shard1DB,
        filter: {op: "update", "command.u.msg": "update_extracted_id_from_query"},
    });

    // Perform an upsert replacement whose query is an exact match on _id and whose replacement
    // doc contains the remainder of the shard key. The _id taken from the query should be used
    // both in targeting the update and in generating the new document.
    writeRes = assert.commandWorked(
        mongosColl.update({_id: 101}, {a: 101, msg: "upsert_extracted_id_from_query"}, {upsert: true}),
    );
    assert.eq(writeRes.nUpserted, 1);

    // Verify that the update only targeted shard1, and that the resulting document appears as
    // expected. At this point in the test we expect shard1 to be stale, because it was the
    // destination shard for the first moveChunk; we therefore explicitly check the profiler for
    // a successful update, i.e. one which did not report a stale config exception.
    assert.docEq([{_id: 101, a: 101, msg: "upsert_extracted_id_from_query"}], mongosColl.find({_id: 101}).toArray());
    assert.docEq([{_id: 101, a: 101, msg: "upsert_extracted_id_from_query"}], shard1DB.test.find({_id: 101}).toArray());
    profilerHasZeroMatchingEntriesOrThrow({
        profileDB: shard0DB,
        filter: {op: "update", "command.u.msg": "upsert_extracted_id_from_query"},
    });
    profilerHasSingleMatchingEntryOrThrow({
        profileDB: shard1DB,
        filter: {
            op: "update",
            "command.u.msg": "upsert_extracted_id_from_query",
            errName: {$exists: false},
        },
    });
}

function runReplacementUpdateTestsForCompoundShardKey() {
    setUpData();

    // Perform a replacement update whose query is an exact match on _id and whose replacement
    // document contains the remainder of the shard key. Despite the fact that the replacement
    // document does not contain the entire shard key, we expect that mongoS will extract the
    // _id from the query and combine it with the replacement doc to target a single shard.
    let writeRes = assert.commandWorked(
        mongosColl.update({_id: -100}, {a: -100, msg: "update_extracted_id_from_query"}),
    );

    // Verify that the update did not modify the orphan document.
    assert.docEq([{_id: -100, a: -100, msg: "not_updated"}], shard1DB.test.find({_id: -100}).toArray());
    assert.eq(writeRes.nMatched, 1);
    assert.eq(writeRes.nModified, 1);

    // Verify that the update only targeted shard0 and that the resulting document appears as
    // expected.
    assert.docEq([{_id: -100, a: -100, msg: "update_extracted_id_from_query"}], mongosColl.find({_id: -100}).toArray());
    profilerHasSingleMatchingEntryOrThrow({
        profileDB: shard0DB,
        filter: {op: "update", "command.u.msg": "update_extracted_id_from_query"},
    });
    profilerHasZeroMatchingEntriesOrThrow({
        profileDB: shard1DB,
        filter: {op: "update", "command.u.msg": "update_extracted_id_from_query"},
    });

    // Verify that an update whose query contains an exact match on _id but whose replacement
    // doc does not contain all other shard key fields will be targeted as if the missing shard
    // key values are null, but will write the replacement document as-is.

    // Need to start a session to change the shard key.
    const session = st.s.startSession({retryWrites: true});
    const sessionDB = session.getDatabase(jsTestName());
    const sessionColl = sessionDB.test;

    sessionColl.insert({_id: -99, a: null, msg: "not_updated"});

    assert.commandWorked(sessionColl.update({_id: -99}, {_id: -99, msg: "update_missing_shard_key_field"}));

    assert.docEq([{_id: -99, msg: "update_missing_shard_key_field"}], sessionColl.find({_id: -99}).toArray());

    // Verify that an upsert whose query contains an exact match on _id but whose replacement
    // document does not contain all other shard key fields will work properly.
    assert.commandWorked(sessionColl.update({_id: -100, a: -100}, {msg: "upsert_targeting_worked"}, {upsert: true}));
    assert.eq(mongosColl.find({_id: -100, a: -100}).itcount(), 0);
    assert.eq(mongosColl.find({msg: "upsert_targeting_worked"}).itcount(), 1);
}

// Shard the test collection on {_id: 1, a: 1}, split it into two chunks, and migrate one of
// these to the second shard.
st.shardColl(mongosColl, {_id: 1, a: 1}, {_id: 0, a: 0}, {_id: 1, a: 1}, mongosDB.getName(), true);

// Run the replacement behaviour tests that are relevant to a compound key that includes _id.
runReplacementUpdateTestsForCompoundShardKey();

// Drop and reshard the collection on {_id: "hashed"}, which will spread chunks across both shards.
assert(mongosColl.drop());
mongosDB.adminCommand({shardCollection: mongosColl.getFullName(), key: {_id: "hashed"}});

// Run the replacement behaviour tests relevant to a collection sharded on {_id: "hashed"}.
runReplacementUpdateTestsForHashedShardKey();

st.stop();
