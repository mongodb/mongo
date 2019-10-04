//
// Tests that documents in a sharded collection with missing shard key fields are treated as if they
// contain an explicit null value for any missing fields.
//
// @tags: [requires_find_command, uses_transactions, uses_multi_shard_transactions]

(function() {
'use strict';

const st = new ShardingTest({shards: 2});
const mongos = st.s0;
const primaryShard = st.shard0.shardName;
const secondaryShard = st.shard1.shardName;
const kDbName = 'db';
const kCollName = 'foo';
const kNsName = kDbName + '.' + kCollName;
const kOldKeyDoc = {
    a: 1
};
const kNewKeyDoc = {
    a: 1,
    b: 1
};

function orphanDocCount() {
    // Since count() includes orphaned documents while find({}).itcount() excludes them, their
    // difference corresponds to the number of orphaned documents in 'db.foo'.
    return mongos.getCollection(kNsName).count() - mongos.getCollection(kNsName).find({}).itcount();
}

function dropAndReshardColl() {
    assert.commandWorked(mongos.getDB(kDbName).runCommand({drop: kCollName}));
    assert.commandWorked(mongos.adminCommand({shardCollection: kNsName, key: kOldKeyDoc}));
    assert.commandWorked(mongos.getCollection(kNsName).createIndex(kNewKeyDoc));

    // Insert six documents such that three correspond to the old shard key and three correspond to
    // the new shard key. Verify that there are no orphaned documents in 'db.foo'.
    assert.commandWorked(mongos.getCollection(kNsName).insert({a: 1}));
    assert.commandWorked(mongos.getCollection(kNsName).insert({a: 10}));
    assert.commandWorked(mongos.getCollection(kNsName).insert({a: null}));
    assert.commandWorked(mongos.getCollection(kNsName).insert({a: 1, b: 1}));
    assert.commandWorked(mongos.getCollection(kNsName).insert({a: 10, b: 1}));
    assert.commandWorked(mongos.getCollection(kNsName).insert({a: null, b: 1}));
    assert.eq(0, orphanDocCount());
}

function isOwnedByShard(shardName, doc) {
    let isOwned = false;
    mongos.getCollection(kNsName)
        .find(doc)
        .explain('executionStats')
        .executionStats.executionStages.shards.forEach((shard) => {
            if (shard.shardName.localeCompare(shardName) === 0) {
                isOwned = (1 === shard.nReturned);
            }
        });
    return isOwned;
}

function isOwnedByPrimaryShard(doc) {
    return isOwnedByShard(primaryShard, doc);
}

function isOwnedBySecondaryShard(doc) {
    return isOwnedByShard(secondaryShard, doc);
}

assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));
st.ensurePrimaryShard(kDbName, primaryShard);

jsTestLog('********** ORPHAN FILTERING **********');

dropAndReshardColl();

// Verify that moving a chunk to the secondary shard produces two orphaned documents in 'db.foo' as
// a result of migrating documents {a: 10} and {a: 10, b: 1}.
assert.commandWorked(mongos.adminCommand({split: kNsName, middle: {a: 5}}));
assert.commandWorked(mongos.adminCommand({moveChunk: kNsName, find: {a: 5}, to: secondaryShard}));
assert.lte(orphanDocCount(), 2);

// Verify that refining the shard key produces no additional orphaned documents in 'db.foo'.
assert.commandWorked(mongos.adminCommand({refineCollectionShardKey: kNsName, key: kNewKeyDoc}));
assert.lte(orphanDocCount(), 2);

jsTestLog('********** REQUEST TARGETING **********');

dropAndReshardColl();

// Ensure that there exist two chunks belonging to 'db.foo' covering the entire key range.
//
// Chunk 1: {a: MinKey, b: MinKey} -->> {a: 5, b: MinKey} (belongs to the primary shard)
// Chunk 2: {a: 5, b: MinKey} -->> {a: MaxKey, b: MaxKey} (belongs to the secondary shard)
assert.commandWorked(mongos.adminCommand({split: kNsName, middle: {a: 5}}));
assert.commandWorked(mongos.adminCommand({moveChunk: kNsName, find: {a: 5}, to: secondaryShard}));
assert.commandWorked(mongos.adminCommand({refineCollectionShardKey: kNsName, key: kNewKeyDoc}));

// Verify that chunk 1 owns documents {a: 1}, {a: null}, {a: 1, b: 1}, {a: null, b: 1} while chunk 2
// owns documents {a: 10} and {a: 10, b: 1}.
assert(isOwnedByPrimaryShard({a: 1, b: {$exists: false}}));
assert(isOwnedBySecondaryShard({a: 10, b: {$exists: false}}));
assert(isOwnedByPrimaryShard({a: null, b: {$exists: false}}));
assert(isOwnedByPrimaryShard({a: 1, b: 1}));
assert(isOwnedBySecondaryShard({a: 10, b: 1}));
assert(isOwnedByPrimaryShard({a: null, b: 1}));

// Verify that find targets shards without treating missing shard key fields as null values.
let docsArr = mongos.getCollection(kNsName).find({b: 1}, {_id: 0}).sort({a: 1}).toArray();
assert.eq(3, docsArr.length);
assert.eq({a: null, b: 1}, docsArr[0]);
assert.eq({a: 1, b: 1}, docsArr[1]);
assert.eq({a: 10, b: 1}, docsArr[2]);

// Verify that count targets shards without treating missing shard key fields as null values.
assert.eq(3, mongos.getCollection(kNsName).count({b: 1}));

// Verify that distinct targets shards without treating missing shard key fields as null values.
const valuesArr = mongos.getCollection(kNsName).distinct('a').sort();
assert.eq(3, valuesArr.length);
assert.eq(1, valuesArr[0]);
assert.eq(10, valuesArr[1]);
assert.eq(null, valuesArr[2]);

// Verify that insert targets shards as if missing shard key fields were null values.
assert.commandWorked(mongos.getCollection(kNsName).insert({b: 10}));
docsArr = mongos.getCollection(kNsName).find({b: 10}).toArray();
assert(isOwnedByPrimaryShard({b: 10}));
assert(!isOwnedBySecondaryShard({b: 10}));

// Verify that delete targets shards without treating missing shard key fields as null values.
assert.commandWorked(mongos.getCollection(kNsName).insert({a: 10, b: 10}));
assert(!isOwnedByPrimaryShard({a: 10, b: 10}));
assert(isOwnedBySecondaryShard({a: 10, b: 10}));

assert.commandWorked(mongos.getCollection(kNsName).remove({b: 10}));
docsArr = mongos.getCollection(kNsName).find({b: 10}, {_id: 0}).toArray();
assert.eq(0, docsArr.length);

// Verify that a query-style update targets shards without treating missing shard key fields as null
// values.
assert.commandWorked(mongos.getCollection(kNsName).update({b: 1}, {$set: {c: 1}}, {multi: true}));
docsArr = mongos.getCollection(kNsName).find({c: 1}, {_id: 0}).sort({a: 1}).toArray();
assert.eq(3, docsArr.length);
assert.eq({a: null, b: 1, c: 1}, docsArr[0]);
assert.eq({a: 1, b: 1, c: 1}, docsArr[1]);
assert.eq({a: 10, b: 1, c: 1}, docsArr[2]);

// Verify that a replacement update targets shards while treating missing shard keys as null values.

// Insert documents all with {d: 1} so they're matched by the update query.
assert.commandWorked(mongos.getCollection(kNsName).insert({a: -100, b: 1, c: 1, d: 1}));
assert.commandWorked(mongos.getCollection(kNsName).insert({a: 0, b: 1, c: 2, d: 1}));
assert.commandWorked(mongos.getCollection(kNsName).insert({a: 100, b: 1, c: 3, d: 1}));

// Update via a query that's missing the shard key, in order to force the targeting logic to fall
// back to the replacement document.

// Need to start a session to change the shard key.
const session = st.s.startSession({retryWrites: true});
const sessionDB = session.getDatabase(kDbName);
const sessionColl = sessionDB[kCollName];

assert.commandWorked(sessionColl.update({d: 1}, {b: 1, c: 4, d: 1}));
docsArr = sessionColl.find({c: 4, d: 1}, {_id: 0}).toArray();
assert.eq(1, docsArr.length);
assert.eq({b: 1, c: 4, d: 1}, docsArr[0]);
assert(isOwnedByPrimaryShard({b: 1, c: 4, d: 1}));
assert(!isOwnedBySecondaryShard({b: 1, c: 4, d: 1}));

// Verify that an upsert targets shards without treating missing shard key fields as null values.
// This implies that upsert still requires the entire shard key to be specified in the query.
assert.writeErrorWithCode(
    mongos.getCollection(kNsName).update({b: 1}, {$set: {c: 2}}, {upsert: true}),
    ErrorCodes.ShardKeyNotFound);

st.stop();
})();