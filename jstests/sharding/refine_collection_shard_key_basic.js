//
// Basic tests for refineCollectionShardKey.
//

// Cannot run the filtering metadata check on tests that run refineCollectionShardKey.
TestData.skipCheckShardFilteringMetadata = true;
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {
    flushRoutersAndRefreshShardMetadata
} from "jstests/sharding/libs/sharded_transactions_helpers.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";
import {
    WriteWithoutShardKeyTestUtil
} from "jstests/sharding/updateOne_without_shard_key/libs/write_without_shard_key_test_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

const st = new ShardingTest({
    mongos: 2,
    shards: 2,
    rs: {nodes: 3},
    configOptions:
        {setParameter: {maxTransactionLockRequestTimeoutMillis: ReplSetTest.kDefaultTimeoutMS}}
});

const mongos = st.s0;
const staleMongos = st.s1;
const primaryShard = st.shard0.shardName;
const secondaryShard = st.shard1.shardName;
const kDbName = 'db';
const kCollName = 'foo';
const kNsName = kDbName + '.' + kCollName;
const kConfigCollections = 'config.collections';
const kConfigTags = 'config.tags';
const kUnrelatedName = kDbName + '.bar';
let oldEpoch = null;

function dropAndRecreateColl(keyDoc) {
    assert.commandWorked(mongos.getDB(kDbName).runCommand({drop: kCollName}));
    assert.commandWorked(mongos.getCollection(kNsName).insert(keyDoc));
}

function dropAndReshardColl(keyDoc) {
    assert.commandWorked(mongos.getDB(kDbName).runCommand({drop: kCollName}));
    assert.commandWorked(mongos.adminCommand({shardCollection: kNsName, key: keyDoc}));
}

function dropAndReshardCollUnique(keyDoc) {
    assert.commandWorked(mongos.getDB(kDbName).runCommand({drop: kCollName}));
    assert.commandWorked(
        mongos.adminCommand({shardCollection: kNsName, key: keyDoc, unique: true}));
}

function validateConfigCollections(keyDoc, oldEpoch) {
    const collArr = mongos.getCollection(kConfigCollections).find({_id: kNsName}).toArray();
    assert.eq(1, collArr.length);
    assert.eq(keyDoc, collArr[0].key);
    assert.neq(oldEpoch, collArr[0].lastmodEpoch);
}

function validateConfigCollectionsUnique(unique) {
    const collArr = mongos.getCollection(kConfigCollections).find({_id: kNsName}).toArray();
    assert.eq(1, collArr.length);
    assert.eq(unique, collArr[0].unique);
}

// 1. Assume oldKeyDoc = {a: 1, b: 1} when validating operations before
//    'refineCollectionShardKey'.
// 2. Assume newKeyDoc = {a: 1, b: 1, c: 1, d: 1} when validating operations after
//    'refineCollectionShardKey'.

function setupCRUDBeforeRefine() {
    const session = mongos.startSession({retryWrites: true});
    const sessionDB = session.getDatabase(kDbName);

    // The documents below will be read after refineCollectionShardKey to verify data integrity.
    assert.commandWorked(sessionDB.getCollection(kCollName).insert({a: 5, b: 5, c: 5, d: 5}));
    assert.commandWorked(sessionDB.getCollection(kCollName).insert({a: 10, b: 10, c: 10, d: 10}));
}

function validateCRUDAfterRefine() {
    // Force a refresh on the mongos and each shard because refineCollectionShardKey only triggers
    // best-effort shard refreshes.
    flushRoutersAndRefreshShardMetadata(st, {ns: kNsName});

    const session = mongos.startSession({retryWrites: true});
    const sessionDB = session.getDatabase(kDbName);

    // Verify that documents inserted before refineCollectionShardKey have not been corrupted.
    assert.eq([{a: 5, b: 5, c: 5, d: 5}],
              sessionDB.getCollection(kCollName).find({a: 5}, {_id: 0}).toArray());
    assert.eq([{a: 10, b: 10, c: 10, d: 10}],
              sessionDB.getCollection(kCollName).find({a: 10}, {_id: 0}).toArray());

    // A write with the incomplete shard key is treated as if the missing values are null.

    assert.commandWorked(sessionDB.getCollection(kCollName).insert({a: 1, b: 1}));
    assert.commandWorked(sessionDB.getCollection(kCollName).insert({a: -1, b: -1}));

    assert.neq(null, sessionDB.getCollection(kCollName).findOne({a: 1, b: 1, c: null, d: null}));
    assert.neq(null, sessionDB.getCollection(kCollName).findOne({a: -1, b: -1, c: null, d: null}));

    // Full shard key writes work properly.
    assert.commandWorked(sessionDB.getCollection(kCollName).insert({a: 1, b: 1, c: 1, d: 1}));
    assert.commandWorked(sessionDB.getCollection(kCollName).insert({a: -1, b: -1, c: -1, d: -1}));

    // This enables the feature allows writes to omit the shard key in their queries.
    if (WriteWithoutShardKeyTestUtil.isWriteWithoutShardKeyFeatureEnabled(sessionDB)) {
        assert.commandWorked(
            sessionDB.getCollection(kCollName).update({a: 1, b: 1, c: 1}, {$set: {x: 2}}));
        assert.commandWorked(
            sessionDB.getCollection(kCollName).update({a: 1, b: 1, c: 1, d: 1}, {$set: {b: 2}}));
        assert.commandWorked(sessionDB.getCollection(kCollName).update({a: -1, b: -1, c: -1, d: -1},
                                                                       {$set: {b: 4}}));

        assert.eq(2, sessionDB.getCollection(kCollName).findOne({c: 1}).x);
        assert.eq(2, sessionDB.getCollection(kCollName).findOne({c: 1}).b);
        assert.eq(4, sessionDB.getCollection(kCollName).findOne({c: -1}).b);

        // Versioned reads against secondaries should work as expected.
        mongos.setReadPref("secondary");
        assert.eq(2, sessionDB.getCollection(kCollName).findOne({c: 1}).x);
        assert.eq(2, sessionDB.getCollection(kCollName).findOne({c: 1}).b);
        assert.eq(4, sessionDB.getCollection(kCollName).findOne({c: -1}).b);
        mongos.setReadPref(null);
    } else {
        // The full shard key is not required in the resulting document when updating. The full
        // shard key is still required in the query, however.
        assert.commandFailedWithCode(
            sessionDB.getCollection(kCollName).update({a: 1, b: 1, c: 1}, {$set: {b: 2}}), 31025);
        assert.commandWorked(
            sessionDB.getCollection(kCollName).update({a: 1, b: 1, c: 1, d: 1}, {$set: {b: 2}}));
        assert.commandWorked(sessionDB.getCollection(kCollName).update({a: -1, b: -1, c: -1, d: -1},
                                                                       {$set: {b: 4}}));

        assert.eq(2, sessionDB.getCollection(kCollName).findOne({c: 1}).b);
        assert.eq(4, sessionDB.getCollection(kCollName).findOne({c: -1}).b);

        // Versioned reads against secondaries should work as expected.
        mongos.setReadPref("secondary");
        assert.eq(2, sessionDB.getCollection(kCollName).findOne({c: 1}).b);
        assert.eq(4, sessionDB.getCollection(kCollName).findOne({c: -1}).b);
        mongos.setReadPref(null);
    }

    assert.commandWorked(sessionDB.getCollection(kCollName).remove({a: 1, b: 1}, true));
    assert.commandWorked(sessionDB.getCollection(kCollName).remove({a: -1, b: -1}, true));

    assert.commandWorked(sessionDB.getCollection(kCollName).remove({a: 1, b: 2, c: 1, d: 1}, true));
    assert.commandWorked(
        sessionDB.getCollection(kCollName).remove({a: -1, b: 4, c: -1, d: -1}, true));
    assert.commandWorked(sessionDB.getCollection(kCollName).remove({a: 5, b: 5, c: 5, d: 5}, true));
    assert.commandWorked(
        sessionDB.getCollection(kCollName).remove({a: 10, b: 10, c: 10, d: 10}, true));
    assert.commandWorked(
        sessionDB.getCollection(kCollName).remove({a: 1, b: 1, c: null, d: null}, true));
    assert.commandWorked(
        sessionDB.getCollection(kCollName).remove({a: -1, b: -1, c: null, d: null}, true));

    assert.eq(null, sessionDB.getCollection(kCollName).findOne());
}

function validateSplitAfterRefine() {
    // The full shard key is required when manually specifying bounds.
    assert.commandFailed(mongos.adminCommand({split: kNsName, middle: {a: 0, b: 0}}));
    assert.commandWorked(mongos.adminCommand({split: kNsName, middle: {a: 0, b: 0, c: 0, d: 0}}));
}

function validateMoveAfterRefine() {
    // The full shard key is required when manually specifying bounds.
    assert.commandFailed(
        mongos.adminCommand({moveChunk: kNsName, find: {a: 5, b: 5}, to: secondaryShard}));
    assert.commandWorked(mongos.adminCommand(
        {moveChunk: kNsName, find: {a: 5, b: 5, c: 5, d: 5}, to: secondaryShard}));
}

function validateMergeAfterRefine() {
    assert.commandWorked(mongos.adminCommand({split: kNsName, middle: {a: 0, b: 0, c: 0, d: 0}}));
    assert.commandWorked(
        mongos.adminCommand({split: kNsName, middle: {a: 10, b: 10, c: 10, d: 10}}));

    // The full shard key is required when manually specifying bounds.
    assert.commandFailed(mongos.adminCommand(
        {mergeChunks: kNsName, bounds: [{a: MinKey, b: MinKey}, {a: MaxKey, b: MaxKey}]}));
    assert.commandWorked(mongos.adminCommand({
        mergeChunks: kNsName,
        bounds: [
            {a: MinKey, b: MinKey, c: MinKey, d: MinKey},
            {a: MaxKey, b: MaxKey, c: MaxKey, d: MaxKey}
        ]
    }));
}

function setupConfigChunksBeforeRefine() {
    // Ensure there exist 2 chunks that are not the global max chunk to properly verify the
    // correctness of the multi-update in refineCollectionShardKey.
    assert.commandWorked(mongos.adminCommand({split: kNsName, middle: {a: 0, b: 0}}));
    assert.commandWorked(mongos.adminCommand({split: kNsName, middle: {a: 5, b: 5}}));

    return findChunksUtil.findOneChunkByNs(mongos.getDB('config'), kNsName).lastmodEpoch;
}

function validateConfigChunksAfterRefine(oldEpoch) {
    const chunkArr =
        findChunksUtil.findChunksByNs(mongos.getDB('config'), kNsName).sort({min: 1}).toArray();
    assert.eq(3, chunkArr.length);
    assert.eq({a: MinKey, b: MinKey, c: MinKey, d: MinKey}, chunkArr[0].min);
    assert.eq({a: 0, b: 0, c: MinKey, d: MinKey}, chunkArr[0].max);
    assert.eq({a: 0, b: 0, c: MinKey, d: MinKey}, chunkArr[1].min);
    assert.eq({a: 5, b: 5, c: MinKey, d: MinKey}, chunkArr[1].max);
    assert.eq({a: 5, b: 5, c: MinKey, d: MinKey}, chunkArr[2].min);
    assert.eq({a: MaxKey, b: MaxKey, c: MaxKey, d: MaxKey}, chunkArr[2].max);
    assert.eq(chunkArr[0].lastmodEpoch, chunkArr[1].lastmodEpoch);
    assert.eq(chunkArr[1].lastmodEpoch, chunkArr[2].lastmodEpoch);
    assert(!oldEpoch && !chunkArr[0].lastmodEpoch || oldEpoch != chunkArr[0].lastmodEpoch);
}

function setupConfigTagsBeforeRefine() {
    // Ensure there exist 2 tags that are not the global max tag to properly verify the
    // correctness of the multi-update in refineCollectionShardKey.
    assert.commandWorked(mongos.adminCommand({addShardToZone: primaryShard, zone: 'zone_1'}));
    assert.commandWorked(mongos.adminCommand({addShardToZone: primaryShard, zone: 'zone_2'}));
    assert.commandWorked(mongos.adminCommand({addShardToZone: primaryShard, zone: 'zone_3'}));
    assert.commandWorked(mongos.adminCommand({
        updateZoneKeyRange: kNsName,
        min: {a: MinKey, b: MinKey},
        max: {a: 0, b: 0},
        zone: 'zone_1'
    }));
    assert.commandWorked(mongos.adminCommand(
        {updateZoneKeyRange: kNsName, min: {a: 0, b: 0}, max: {a: 5, b: 5}, zone: 'zone_2'}));
    assert.commandWorked(mongos.adminCommand({
        updateZoneKeyRange: kNsName,
        min: {a: 5, b: 5},
        max: {a: MaxKey, b: MaxKey},
        zone: 'zone_3'
    }));
}

function validateConfigTagsAfterRefine() {
    const tagsArr = mongos.getCollection(kConfigTags).find({ns: kNsName}).sort({min: 1}).toArray();
    assert.eq(3, tagsArr.length);
    assert.eq({a: MinKey, b: MinKey, c: MinKey, d: MinKey}, tagsArr[0].min);
    assert.eq({a: 0, b: 0, c: MinKey, d: MinKey}, tagsArr[0].max);
    assert.eq({a: 0, b: 0, c: MinKey, d: MinKey}, tagsArr[1].min);
    assert.eq({a: 5, b: 5, c: MinKey, d: MinKey}, tagsArr[1].max);
    assert.eq({a: 5, b: 5, c: MinKey, d: MinKey}, tagsArr[2].min);
    assert.eq({a: MaxKey, b: MaxKey, c: MaxKey, d: MaxKey}, tagsArr[2].max);
}

function validateUnrelatedCollAfterRefine(oldCollArr, oldChunkArr, oldTagsArr) {
    const collArr = mongos.getCollection(kConfigCollections).find({_id: kUnrelatedName}).toArray();
    assert.eq(1, collArr.length);
    assert.sameMembers(oldCollArr, collArr);

    const chunkArr =
        findChunksUtil.findChunksByNs(mongos.getDB('config'), kUnrelatedName).toArray();
    assert.eq(3, chunkArr.length);
    assert.sameMembers(oldChunkArr, chunkArr);

    const tagsArr = mongos.getCollection(kConfigTags).find({ns: kUnrelatedName}).toArray();
    assert.eq(3, tagsArr.length);
    assert.sameMembers(oldTagsArr, tagsArr);
}

jsTestLog('********** SIMPLE TESTS **********');

var result;

// Should fail because arguments 'refineCollectionShardKey' and 'key' are invalid types.
assert.commandFailedWithCode(
    mongos.adminCommand({refineCollectionShardKey: {_id: 1}, key: {_id: 1, aKey: 1}}),
    ErrorCodes.TypeMismatch);
assert.commandFailedWithCode(mongos.adminCommand({refineCollectionShardKey: kNsName, key: 'blah'}),
                             ErrorCodes.TypeMismatch);

// Should fail because refineCollectionShardKey may only be run against the admin database.
assert.commandFailedWithCode(
    mongos.getDB(kDbName).runCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}}),
    ErrorCodes.Unauthorized);

// Should fail because namespace 'db.foo' does not exist.
assert.commandFailedWithCode(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}}),
    ErrorCodes.NamespaceNotFound);

assert.commandWorked(mongos.getCollection(kNsName).insert({aKey: 1}));

// Should fail because namespace 'db.foo' is not sharded.
assert.commandFailedWithCode(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}}),
    ErrorCodes.NamespaceNotSharded);

assert.commandFailedWithCode(
    mongos.adminCommand({refineCollectionShardKey: "config.collections", key: {_id: 1, aKey: 1}}),
    ErrorCodes.NamespaceNotSharded);

assert.commandWorked(mongos.adminCommand({shardCollection: kNsName, key: {_id: 1}}));

// Should fail because shard key is invalid (i.e. bad values).
assert.commandFailedWithCode(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 5}}), ErrorCodes.BadValue);
assert.commandFailedWithCode(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: -1}}), ErrorCodes.BadValue);
assert.commandFailedWithCode(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 'hashed', aKey: 'hashed'}}),
    ErrorCodes.BadValue);
assert.commandFailedWithCode(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {aKey: 'hahashed'}}),
    ErrorCodes.BadValue);

// Should fail because shard key is not specified.
assert.commandFailedWithCode(mongos.adminCommand({refineCollectionShardKey: kNsName}),
                             ErrorCodes.IDLFailedToParse);
assert.commandFailedWithCode(mongos.adminCommand({refineCollectionShardKey: kNsName, key: {}}),
                             ErrorCodes.BadValue);

// Should work because new shard key is already same as current shard key of namespace 'db.foo'.
assert.commandWorked(mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1}}));
dropAndReshardColl({a: 1, b: 1});
assert.commandWorked(mongos.adminCommand({refineCollectionShardKey: kNsName, key: {a: 1, b: 1}}));
dropAndReshardColl({aKey: 'hashed'});
assert.commandWorked(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {aKey: 'hashed'}}));
dropAndReshardColl({_id: 1, aKey: 'hashed'});
assert.commandWorked(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 'hashed'}}));

assert.commandWorked(mongos.getDB(kDbName).dropDatabase());

jsTestLog('********** NAMESPACE VALIDATION TESTS **********');

assert.commandWorked(mongos.adminCommand({shardCollection: kNsName, key: {_id: 1}}));

// Configure failpoint 'hangRefineCollectionShardKeyAfterRefresh' on staleMongos and run
// refineCollectionShardKey against this mongos in a parallel thread.
let hangAfterRefreshFailPoint =
    configureFailPoint(staleMongos, 'hangRefineCollectionShardKeyAfterRefresh');
const awaitShellToTriggerNamespaceNotSharded = startParallelShell(() => {
    assert.commandFailedWithCode(
        db.adminCommand({refineCollectionShardKey: 'db.foo', key: {_id: 1, aKey: 1}}),
        ErrorCodes.NamespaceNotSharded);
}, staleMongos.port);
hangAfterRefreshFailPoint.wait();

// Drop and re-create namespace 'db.foo' without staleMongos refreshing its metadata.
dropAndRecreateColl({aKey: 1});

// Should fail because namespace 'db.foo' is not sharded. NOTE: This NamespaceNotSharded error
// is thrown in ConfigsvrRefineCollectionShardKeyCommand.
hangAfterRefreshFailPoint.off();
awaitShellToTriggerNamespaceNotSharded();

assert.commandWorked(mongos.getDB(kDbName).dropDatabase());

jsTestLog('********** SHARD KEY VALIDATION TESTS **********');

assert.commandWorked(mongos.adminCommand({shardCollection: kNsName, key: {_id: 1}}));

// Should fail because new shard key {aKey: 1} does not extend current shard key {_id: 1} of
// namespace 'db.foo'.
assert.commandFailedWithCode(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {aKey: 1}}),
    ErrorCodes.InvalidOptions);

// Should fail because no index exists for new shard key {_id: 1, aKey: 1}.
assert.commandFailedWithCode(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}}),
    ErrorCodes.InvalidOptions);

// Should fail because only a sparse index exists for new shard key {_id: 1, aKey: 1}.
dropAndReshardColl({_id: 1});
assert.commandWorked(mongos.getCollection(kNsName).createIndex({_id: 1, aKey: 1}, {sparse: true}));

result = mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}});
assert.commandFailedWithCode(result, ErrorCodes.InvalidOptions);
assert(result.errmsg.includes("Index key is sparse."));

// Should fail because index has a non-simple collation.
dropAndReshardColl({aKey: 1});
assert.commandWorked(mongos.getCollection(kNsName).createIndex({aKey: 1, bKey: 1}, {
    collation: {
        locale: "en",
    }
}));
result = mongos.adminCommand({refineCollectionShardKey: kNsName, key: {aKey: 1, bKey: 1}});
assert.commandFailedWithCode(result, ErrorCodes.InvalidOptions);
assert(result.errmsg.includes("Index has a non-simple collation."));

// Should fail because only a partial index exists for new shard key {_id: 1, aKey: 1}.
dropAndReshardColl({_id: 1});
assert.commandWorked(mongos.getCollection(kNsName).createIndex(
    {_id: 1, aKey: 1}, {partialFilterExpression: {aKey: {$gt: 0}}}));

result = mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}});
assert.commandFailedWithCode(result, ErrorCodes.InvalidOptions);
assert(result.errmsg.includes("Index key is partial."));

// Should fail because only a multikey index exists for new shard key {_id: 1, aKey: 1}.
dropAndReshardColl({_id: 1});
assert.commandWorked(mongos.getCollection(kNsName).createIndex({_id: 1, aKey: 1}));
assert.commandWorked(mongos.getCollection(kNsName).insert({aKey: [1, 2, 3, 4, 5]}));

result = mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}});
assert.commandFailedWithCode(result, ErrorCodes.InvalidOptions);
assert(result.errmsg.includes("Index key is multikey."));

// Should fail because current shard key {a: 1} is unique, new shard key is {a: 1, b: 1}, and an
// index only exists on {a: 1, b: 1, c: 1}.
dropAndReshardCollUnique({a: 1});
assert.commandWorked(mongos.getCollection(kNsName).createIndex({a: 1, b: 1, c: 1}));

mongos.adminCommand({refineCollectionShardKey: kNsName, key: {a: 1, b: 1}});
assert.commandFailedWithCode(result, ErrorCodes.InvalidOptions);

// Should work because current shard key {_id: 1} is not unique, new shard key is {_id: 1, aKey:
// 1}, and an index exists on {_id: 1, aKey: 1, bKey: 1}.
dropAndReshardColl({_id: 1});
assert.commandWorked(mongos.getCollection(kNsName).createIndex({_id: 1, aKey: 1, bKey: 1}));
oldEpoch = mongos.getCollection(kConfigCollections).findOne({_id: kNsName}).lastmodEpoch;

assert.commandWorked(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}}));
validateConfigCollections({_id: 1, aKey: 1}, oldEpoch);

// Should work because an index with missing or incomplete shard key entries exists for new shard
// key {_id: 1, aKey: 1} and these entries are treated as null values.
dropAndReshardColl({_id: 1});
assert.commandWorked(mongos.getCollection(kNsName).createIndex({_id: 1, aKey: 1}));
assert.commandWorked(mongos.getCollection(kNsName).insert({_id: 12345}));

assert.commandWorked(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}}));

// Should work because an index with missing or incomplete shard key entries exists for new shard
// key {_id: "hashed", aKey: 1} and these entries are treated as null values.
dropAndReshardColl({_id: "hashed"});
assert.commandWorked(mongos.getCollection(kNsName).createIndex({_id: "hashed", aKey: 1}));
assert.commandWorked(mongos.getCollection(kNsName).insert({_id: 12345}));

assert.commandWorked(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: "hashed", aKey: 1}}));

// Should work because an index with missing or incomplete shard key entries exists for new shard
// key {_id: 1, aKey: "hashed"} and these entries are treated as null values.
dropAndReshardColl({_id: 1});
assert.commandWorked(mongos.getCollection(kNsName).createIndex({_id: 1, aKey: "hashed"}));
assert.commandWorked(mongos.getCollection(kNsName).insert({_id: 12345}));

assert.commandWorked(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: "hashed"}}));

// Should fail because new shard key {aKey: 1} is not a prefix of current shard key {_id: 1,
// aKey: 1}.
dropAndReshardColl({_id: 1, aKey: 1});
assert.commandWorked(mongos.getCollection(kNsName).createIndex({aKey: 1}));

assert.commandFailedWithCode(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {aKey: 1}}),
    ErrorCodes.InvalidOptions);

// Should fail because new shard key {aKey: 1, _id: 1} is not a prefix of current shard key
// {_id: 1, aKey: 1}.
dropAndReshardColl({_id: 1, aKey: 1});
assert.commandWorked(mongos.getCollection(kNsName).createIndex({aKey: 1, _id: 1}));

assert.commandFailedWithCode(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {aKey: 1, _id: 1}}),
    ErrorCodes.InvalidOptions);

// Should fail because new shard key {aKey: 1, _id: 1, bKey: 1} is not a prefix of current shard
// key {_id: 1, aKey: 1}.
dropAndReshardColl({_id: 1, aKey: 1});
assert.commandWorked(mongos.getCollection(kNsName).createIndex({aKey: 1, _id: 1, bKey: 1}));

assert.commandFailedWithCode(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {aKey: 1, _id: 1, bKey: 1}}),
    ErrorCodes.InvalidOptions);

// Should fail because new shard key {aKey: 1, bKey: 1} is not a prefix of current shard key
// {_id: 1}.
dropAndReshardColl({_id: 1});
assert.commandWorked(mongos.getCollection(kNsName).createIndex({aKey: 1, bKey: 1}));

assert.commandFailedWithCode(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {aKey: 1, bKey: 1}}),
    ErrorCodes.InvalidOptions);

// Should fail because index key is sparse and index has non-simple collation.
dropAndReshardColl({_id: 1});
assert.commandWorked(mongos.getCollection(kNsName).createIndex({_id: 1, aKey: 1}, {
    sparse: true,
    collation: {
        locale: "en",
    }
}));
result = mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}});
assert.commandFailedWithCode(result, ErrorCodes.InvalidOptions);
assert(result.errmsg.includes("Index key is sparse.") &&
       result.errmsg.includes("Index has a non-simple collation."));

// Should fail because index key is multikey and is partial.
dropAndReshardColl({_id: 1});
assert.commandWorked(mongos.getCollection(kNsName).createIndex(
    {_id: 1, aKey: 1}, {name: "index_1_part", partialFilterExpression: {aKey: {$gt: 0}}}));
assert.commandWorked(
    mongos.getCollection(kNsName).createIndex({_id: 1, aKey: 1}, {name: "index_2"}));
assert.commandWorked(mongos.getCollection(kNsName).insert({aKey: [1, 2, 3, 4, 5]}));

result = mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}});
assert.commandFailedWithCode(result, ErrorCodes.InvalidOptions);
assert(result.errmsg.includes("Index key is multikey.") &&
       result.errmsg.includes("Index key is partial."));

// Should fail because both indexes have keys that are incompatible: partial; sparse
dropAndReshardColl({_id: 1});
assert.commandWorked(mongos.getCollection(kNsName).createIndex(
    {_id: 1, aKey: 1}, {name: "index_1_part", partialFilterExpression: {aKey: {$gt: 0}}}));
assert.commandWorked(mongos.getCollection(kNsName).createIndex(
    {_id: 1, aKey: 1}, {name: "index_2_sparse", sparse: true}));
result = mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}});
assert.commandFailedWithCode(result, ErrorCodes.InvalidOptions);
assert(result.errmsg.includes("Index key is partial.") &&
       result.errmsg.includes("Index key is sparse."));

// Should work because a 'useful' index exists for new shard key {_id: 1, aKey: 1}.
dropAndReshardColl({_id: 1});
assert.commandWorked(mongos.getCollection(kNsName).createIndex({_id: 1, aKey: 1}));
oldEpoch = mongos.getCollection(kConfigCollections).findOne({_id: kNsName}).lastmodEpoch;

assert.commandWorked(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}}));
validateConfigCollections({_id: 1, aKey: 1}, oldEpoch);

// Should work because a 'useful' index exists for new shard key {a: 1, b.c: 1}. NOTE: We are
// explicitly verifying that refineCollectionShardKey works with a dotted field.
dropAndReshardColl({a: 1});
assert.commandWorked(mongos.adminCommand({split: kNsName, middle: {a: 0}}));
assert.commandWorked(mongos.getCollection(kNsName).createIndex({a: 1, 'b.c': 1}));
oldEpoch = mongos.getCollection(kConfigCollections).findOne({_id: kNsName}).lastmodEpoch;

assert.commandWorked(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {a: 1, 'b.c': 1}}));
assert.commandWorked(mongos.adminCommand({split: kNsName, middle: {a: 0, 'b.c': 0}}));
validateConfigCollections({a: 1, 'b.c': 1}, oldEpoch);

// Refining a shard key with a dotted field to include more dotted fields should work.
dropAndReshardColl({a: 1, 'b.c': 1});
assert.commandWorked(mongos.adminCommand({split: kNsName, middle: {a: 0, 'b.c': 0}}));
assert.commandWorked(
    mongos.getCollection(kNsName).createIndex({a: 1, 'b.c': 1, d: 1, 'e.f.g': 1, h: 1}));
oldEpoch = mongos.getCollection(kConfigCollections).findOne({_id: kNsName}).lastmodEpoch;

assert.commandWorked(mongos.adminCommand(
    {refineCollectionShardKey: kNsName, key: {a: 1, 'b.c': 1, d: 1, 'e.f.g': 1, h: 1}}));
assert.commandWorked(
    mongos.adminCommand({split: kNsName, middle: {a: 0, 'b.c': 0, d: 0, 'e.f.g': 0, h: 0}}));
validateConfigCollections({a: 1, 'b.c': 1, d: 1, 'e.f.g': 1, h: 1}, oldEpoch);

// Refining a shard key with a dotted field to include a non-dotted field should work.
dropAndReshardColl({'a.b': 1});
assert.commandWorked(mongos.adminCommand({split: kNsName, middle: {'a.b': 0}}));
assert.commandWorked(mongos.getCollection(kNsName).createIndex({'a.b': 1, c: 1}));
oldEpoch = mongos.getCollection(kConfigCollections).findOne({_id: kNsName}).lastmodEpoch;

assert.commandWorked(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {'a.b': 1, c: 1}}));
assert.commandWorked(mongos.adminCommand({split: kNsName, middle: {'a.b': 0, c: 0}}));
validateConfigCollections({'a.b': 1, c: 1}, oldEpoch);

assert.commandWorked(mongos.getDB(kDbName).dropDatabase());

jsTestLog('********** UNIQUENESS PROPERTY TESTS **********');

assert.commandWorked(mongos.adminCommand({shardCollection: kNsName, key: {_id: 1}}));
assert.commandWorked(mongos.getCollection(kNsName).createIndex({_id: 1, aKey: 1}));

// Verify that refineCollectionShardKey cannot modify a unique=false sharded collection.
assert.commandWorked(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}}));
validateConfigCollectionsUnique(false);

// Verify that refineCollectionShardKey cannot modify a unique=true sharded collection.
dropAndReshardCollUnique({_id: 1});
assert.commandWorked(mongos.getCollection(kNsName).createIndex({_id: 1, aKey: 1}));

assert.commandWorked(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}}));
validateConfigCollectionsUnique(true);

// Verify that enforceUniquenessCheck: false allows non-unique indexes.
assert.commandWorked(mongos.getDB(kDbName).runCommand({drop: kCollName}));
assert.commandWorked(mongos.getCollection(kNsName).createIndex({a: 1, b: 1}));
assert.commandWorked(mongos.adminCommand(
    {shardCollection: kNsName, key: {a: 1}, unique: true, enforceUniquenessCheck: false}));
assert.commandWorked(mongos.adminCommand(
    {refineCollectionShardKey: kNsName, key: {a: 1, b: 1}, enforceUniquenessCheck: false}));
validateConfigCollectionsUnique(true);

assert.commandWorked(mongos.getDB(kDbName).dropDatabase());

jsTestLog('********** INTEGRATION TESTS **********');

const oldKeyDoc = {
    a: 1,
    b: 1
};
const newKeyDoc = {
    a: 1,
    b: 1,
    c: 1,
    d: 1
};

assert.commandWorked(mongos.adminCommand({shardCollection: kNsName, key: oldKeyDoc}));
assert.commandWorked(mongos.getCollection(kNsName).createIndex(newKeyDoc));

// CRUD operations before and after refineCollectionShardKey should work as expected.
setupCRUDBeforeRefine();
assert.commandWorked(mongos.adminCommand({refineCollectionShardKey: kNsName, key: newKeyDoc}));
validateCRUDAfterRefine();

// Split chunk operations before and after refineCollectionShardKey should work as expected.
dropAndReshardColl(oldKeyDoc);
assert.commandWorked(mongos.getCollection(kNsName).createIndex(newKeyDoc));

assert.commandWorked(mongos.adminCommand({refineCollectionShardKey: kNsName, key: newKeyDoc}));
validateSplitAfterRefine();

// Move chunk operations before and after refineCollectionShardKey should work as expected.
dropAndReshardColl(oldKeyDoc);
assert.commandWorked(mongos.getCollection(kNsName).createIndex(newKeyDoc));
assert.commandWorked(mongos.adminCommand({split: kNsName, middle: {a: 0, b: 0}}));
assert.commandWorked(mongos.adminCommand({split: kNsName, middle: {a: 10, b: 10}}));

assert.commandWorked(mongos.adminCommand({refineCollectionShardKey: kNsName, key: newKeyDoc}));
validateMoveAfterRefine();

// Merge chunk operations before and after refineCollectionShardKey should work as expected.
dropAndReshardColl(oldKeyDoc);
assert.commandWorked(mongos.getCollection(kNsName).createIndex(newKeyDoc));

assert.commandWorked(mongos.adminCommand({refineCollectionShardKey: kNsName, key: newKeyDoc}));
validateMergeAfterRefine();

// The config.chunks collection before and after refineCollectionShardKey should be as expected.
dropAndReshardColl(oldKeyDoc);
assert.commandWorked(mongos.getCollection(kNsName).createIndex(newKeyDoc));

oldEpoch = setupConfigChunksBeforeRefine();
assert.commandWorked(mongos.adminCommand({refineCollectionShardKey: kNsName, key: newKeyDoc}));
validateConfigChunksAfterRefine(oldEpoch);

// The config.tags collection before and after refineCollectionShardKey should be as expected.
dropAndReshardColl(oldKeyDoc);
assert.commandWorked(mongos.getCollection(kNsName).createIndex(newKeyDoc));

setupConfigTagsBeforeRefine();
assert.commandWorked(mongos.adminCommand({refineCollectionShardKey: kNsName, key: newKeyDoc}));
validateConfigTagsAfterRefine();

// Create an unrelated namespace 'db.bar' with 3 chunks and 3 tags to verify that it isn't
// corrupted after refineCollectionShardKey.
dropAndReshardColl(oldKeyDoc);
assert.commandWorked(mongos.getCollection(kNsName).createIndex(newKeyDoc));

assert.commandWorked(mongos.adminCommand({shardCollection: kUnrelatedName, key: oldKeyDoc}));
assert.commandWorked(mongos.adminCommand({split: kUnrelatedName, middle: {a: 0, b: 0}}));
assert.commandWorked(mongos.adminCommand({split: kUnrelatedName, middle: {a: 5, b: 5}}));
assert.commandWorked(mongos.adminCommand({addShardToZone: primaryShard, zone: 'unrelated_1'}));
assert.commandWorked(mongos.adminCommand({addShardToZone: primaryShard, zone: 'unrelated_2'}));
assert.commandWorked(mongos.adminCommand({addShardToZone: primaryShard, zone: 'unrelated_3'}));
assert.commandWorked(mongos.adminCommand({
    updateZoneKeyRange: kUnrelatedName,
    min: {a: MinKey, b: MinKey},
    max: {a: 0, b: 0},
    zone: 'unrelated_1'
}));
assert.commandWorked(mongos.adminCommand({
    updateZoneKeyRange: kUnrelatedName,
    min: {a: 0, b: 0},
    max: {a: 5, b: 5},
    zone: 'unrelated_2'
}));
assert.commandWorked(mongos.adminCommand({
    updateZoneKeyRange: kUnrelatedName,
    min: {a: 5, b: 5},
    max: {a: MaxKey, b: MaxKey},
    zone: 'unrelated_3'
}));

const oldCollArr = mongos.getCollection(kConfigCollections).find({_id: kUnrelatedName}).toArray();
const oldChunkArr = findChunksUtil.findChunksByNs(mongos.getDB('config'), kUnrelatedName).toArray();
const oldTagsArr = mongos.getCollection(kConfigTags).find({ns: kUnrelatedName}).toArray();
assert.eq(1, oldCollArr.length);
assert.eq(3, oldChunkArr.length);
assert.eq(3, oldTagsArr.length);

assert.commandWorked(mongos.adminCommand({refineCollectionShardKey: kNsName, key: newKeyDoc}));
validateUnrelatedCollAfterRefine(oldCollArr, oldChunkArr, oldTagsArr);

// Verify that all shards in the namespace 'db.foo' eventually refresh. NOTE: This will only succeed
// in a linear jstest without failovers.
const isStepdownSuite = typeof ContinuousStepdown !== 'undefined';
if (!isStepdownSuite) {
    dropAndReshardColl(oldKeyDoc);
    assert.commandWorked(mongos.getCollection(kNsName).createIndex(newKeyDoc));

    assert.commandWorked(
        mongos.adminCommand({moveChunk: kNsName, find: {a: 0, b: 0}, to: secondaryShard}));
    assert.commandWorked(
        mongos.adminCommand({moveChunk: kNsName, find: {a: 0, b: 0}, to: primaryShard}));

    const oldPrimaryEpoch = st.shard0.adminCommand({getShardVersion: kNsName, fullMetadata: true})
                                .metadata.shardVersionEpoch.toString();
    const oldSecondaryEpoch = st.shard1.adminCommand({getShardVersion: kNsName, fullMetadata: true})
                                  .metadata.shardVersionEpoch.toString();

    assert.commandWorked(mongos.adminCommand({refineCollectionShardKey: kNsName, key: newKeyDoc}));

    assert.soon(() => oldPrimaryEpoch !==
                    st.shard0.adminCommand({getShardVersion: kNsName, fullMetadata: true})
                        .metadata.shardVersionEpoch.toString());
    assert.soon(() => oldSecondaryEpoch !==
                    st.shard1.adminCommand({getShardVersion: kNsName, fullMetadata: true})
                        .metadata.shardVersionEpoch.toString());
}

// Assumes the given arrays are sorted by the max field.
function compareMinAndMaxFields(shardedArr, refinedArr) {
    assert(shardedArr.length && refinedArr.length, tojson(shardedArr) + ", " + tojson(refinedArr));
    assert.eq(shardedArr.length, refinedArr.length, tojson(shardedArr) + ", " + tojson(refinedArr));

    const shardedMinAndMax = shardedArr.map(obj => {
        return {min: obj.min, max: obj.max};
    });
    const refinedMinAndMax = refinedArr.map(obj => {
        return {min: obj.min, max: obj.max};
    });
    assert.eq(shardedMinAndMax, refinedMinAndMax);
}

// Verifies the min and max fields are the same for the chunks and tags in the given collections.
function compareBoundaries(conn, shardedNs, refinedNs) {
    // Compare chunks.
    const shardedChunks =
        findChunksUtil.findChunksByNs(conn.getDB("config"), shardedNs).sort({max: 1}).toArray();
    const refinedChunks =
        findChunksUtil.findChunksByNs(conn.getDB("config"), refinedNs).sort({max: 1}).toArray();
    compareMinAndMaxFields(shardedChunks, refinedChunks);

    // Compare tags.
    const shardedTags = conn.getDB("config").tags.find({ns: shardedNs}).sort({max: 1}).toArray();
    const refinedTags = conn.getDB("config").tags.find({ns: refinedNs}).sort({max: 1}).toArray();
    compareMinAndMaxFields(shardedTags, refinedTags);
}

//
// Verify the chunk and tag boundaries are the same for a collection sharded to a certain shard key
// and a collection refined to that same shard key.
//

// For a shard key without nested fields.
(() => {
    const dbName = "compareDB";
    const shardedNs = dbName + ".shardedColl";
    const refinedNs = dbName + ".refinedColl";

    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
    assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: 'zone_1'}));

    assert.commandWorked(st.s.adminCommand({shardCollection: shardedNs, key: {a: 1, b: 1, c: 1}}));
    assert.commandWorked(
        st.s.adminCommand({split: shardedNs, middle: {a: 0, b: MinKey, c: MinKey}}));
    assert.commandWorked(st.s.adminCommand({
        updateZoneKeyRange: shardedNs,
        min: {a: MinKey, b: MinKey, c: MinKey},
        max: {a: 0, b: MinKey, c: MinKey},
        zone: 'zone_1'
    }));
    assert.commandWorked(st.s.adminCommand({
        updateZoneKeyRange: shardedNs,
        min: {a: 10, b: MinKey, c: MinKey},
        max: {a: MaxKey, b: MaxKey, c: MaxKey},
        zone: 'zone_1'
    }));

    assert.commandWorked(st.s.adminCommand({shardCollection: refinedNs, key: {a: 1}}));
    assert.commandWorked(st.s.adminCommand({split: refinedNs, middle: {a: 0}}));
    assert.commandWorked(st.s.adminCommand(
        {updateZoneKeyRange: refinedNs, min: {a: MinKey}, max: {a: 0}, zone: 'zone_1'}));
    assert.commandWorked(st.s.adminCommand(
        {updateZoneKeyRange: refinedNs, min: {a: 10}, max: {a: MaxKey}, zone: 'zone_1'}));

    assert.commandWorked(st.s.getCollection(refinedNs).createIndex({a: 1, b: 1, c: 1}));
    assert.commandWorked(
        st.s.adminCommand({refineCollectionShardKey: refinedNs, key: {a: 1, b: 1, c: 1}}));

    compareBoundaries(st.s, shardedNs, refinedNs);
})();

// Make sure split  is correctly disabled for unsplittable collection
(() => {
    if (FeatureFlagUtil.isPresentAndEnabled(mongos, "TrackUnshardedCollectionsUponCreation")) {
        jsTest.log("Make sure refine shard key for unsplittable collection is correctly disabled");
        const kCollNameUnsplittable = "unsplittable_bar";
        const kNsNameUnsplittable = kDbName + "." + kCollNameUnsplittable;
        assert.commandWorked(mongos.getDB(kDbName).runCommand(
            {createUnsplittableCollection: kCollNameUnsplittable}));
        assert.commandFailedWithCode(
            mongos.adminCommand({refineCollectionShardKey: kNsNameUnsplittable, key: {a: 1, b: 1}}),
            ErrorCodes.NamespaceNotSharded);
    }
})();

// For a shard key with nested fields .
(() => {
    const dbName = "compareDBNested";
    const shardedNs = dbName + ".shardedColl";
    const refinedNs = dbName + ".refinedColl";

    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
    assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: 'zone_1'}));

    assert.commandWorked(
        st.s.adminCommand({shardCollection: shardedNs, key: {"a.b": 1, "c.d.e": 1, f: 1}}));
    assert.commandWorked(
        st.s.adminCommand({split: shardedNs, middle: {"a.b": 0, "c.d.e": MinKey, f: MinKey}}));
    assert.commandWorked(st.s.adminCommand({
        updateZoneKeyRange: shardedNs,
        min: {"a.b": MinKey, "c.d.e": MinKey, f: MinKey},
        max: {"a.b": 0, "c.d.e": MinKey, f: MinKey},
        zone: 'zone_1'
    }));
    assert.commandWorked(st.s.adminCommand({
        updateZoneKeyRange: shardedNs,
        min: {"a.b": 10, "c.d.e": MinKey, f: MinKey},
        max: {"a.b": MaxKey, "c.d.e": MaxKey, f: MaxKey},
        zone: 'zone_1'
    }));

    assert.commandWorked(st.s.adminCommand({shardCollection: refinedNs, key: {"a.b": 1}}));
    assert.commandWorked(st.s.adminCommand({split: refinedNs, middle: {"a.b": 0}}));
    assert.commandWorked(st.s.adminCommand(
        {updateZoneKeyRange: refinedNs, min: {"a.b": MinKey}, max: {"a.b": 0}, zone: 'zone_1'}));
    assert.commandWorked(st.s.adminCommand(
        {updateZoneKeyRange: refinedNs, min: {"a.b": 10}, max: {"a.b": MaxKey}, zone: 'zone_1'}));

    assert.commandWorked(st.s.getCollection(refinedNs).createIndex({"a.b": 1, "c.d.e": 1, f: 1}));
    assert.commandWorked(st.s.adminCommand(
        {refineCollectionShardKey: refinedNs, key: {"a.b": 1, "c.d.e": 1, f: 1}}));

    compareBoundaries(st.s, shardedNs, refinedNs);
})();

st.stop();
