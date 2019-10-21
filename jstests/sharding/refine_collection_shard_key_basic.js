//
// Basic tests for refineCollectionShardKey.
//
// Tag this test as 'requires_find_command' to prevent it from running in the legacy passthroughs
// and as 'requires_document_locking' because it uses retryable writes.
// @tags: [requires_find_command, requires_document_locking]
//

(function() {
'use strict';
load('jstests/sharding/libs/sharded_transactions_helpers.js');

const st = new ShardingTest({mongos: 2, shards: 2, rs: {nodes: 3}});
const mongos = st.s0;
const staleMongos = st.s1;
const primaryShard = st.shard0.shardName;
const secondaryShard = st.shard1.shardName;
const kDbName = 'db';
const kCollName = 'foo';
const kNsName = kDbName + '.' + kCollName;
const kConfigCollections = 'config.collections';
const kConfigChunks = 'config.chunks';
const kConfigTags = 'config.tags';
const kConfigChangelog = 'config.changelog';
const kUnrelatedName = kDbName + '.bar';
let oldEpoch = null;

function enableShardingAndShardColl(keyDoc) {
    assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));
    st.ensurePrimaryShard(kDbName, primaryShard);
    assert.commandWorked(mongos.adminCommand({shardCollection: kNsName, key: keyDoc}));
}

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

function validateConfigChangelog(count) {
    // We allow for more than one entry in the changelog for the 'start' message, because a config
    // server stepdown can cause the command to be retried and the changelog entry to be rewritten.
    assert.gte(count,
               mongos.getCollection(kConfigChangelog)
                   .find({what: 'refineCollectionShardKey.start', ns: kNsName})
                   .itcount());
    assert.lte(count,
               mongos.getCollection(kConfigChangelog)
                   .find({what: 'refineCollectionShardKey.end', ns: kNsName})
                   .itcount());
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

    // The full shard key is not required when updating documents.
    assert.commandWorked(
        sessionDB.getCollection(kCollName).update({a: 1, b: 1, c: 1}, {$set: {b: 2}}));
    assert.commandWorked(
        sessionDB.getCollection(kCollName).update({a: -1, b: -1, c: -1}, {$set: {b: 4}}));

    assert.eq(2, sessionDB.getCollection(kCollName).findOne({c: 1}).b);
    assert.eq(4, sessionDB.getCollection(kCollName).findOne({c: -1}).b);

    // Versioned reads against secondaries should work as expected.
    mongos.setReadPref("secondary");
    assert.eq(2, sessionDB.getCollection(kCollName).findOne({c: 1}).b);
    assert.eq(4, sessionDB.getCollection(kCollName).findOne({c: -1}).b);
    mongos.setReadPref(null);

    // The full shard key is required when removing documents.
    assert.writeErrorWithCode(sessionDB.getCollection(kCollName).remove({a: 1, b: 1}, true),
                              ErrorCodes.ShardKeyNotFound);
    assert.writeErrorWithCode(sessionDB.getCollection(kCollName).remove({a: -1, b: -1}, true),
                              ErrorCodes.ShardKeyNotFound);
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

    return mongos.getCollection(kConfigChunks).findOne({ns: kNsName}).lastmodEpoch;
}

function validateConfigChunksAfterRefine(oldEpoch) {
    const chunkArr =
        mongos.getCollection(kConfigChunks).find({ns: kNsName}).sort({min: 1}).toArray();
    assert.eq(3, chunkArr.length);
    assert.eq({a: MinKey, b: MinKey, c: MinKey, d: MinKey}, chunkArr[0].min);
    assert.eq({a: 0, b: 0, c: MinKey, d: MinKey}, chunkArr[0].max);
    assert.eq({a: 0, b: 0, c: MinKey, d: MinKey}, chunkArr[1].min);
    assert.eq({a: 5, b: 5, c: MinKey, d: MinKey}, chunkArr[1].max);
    assert.eq({a: 5, b: 5, c: MinKey, d: MinKey}, chunkArr[2].min);
    assert.eq({a: MaxKey, b: MaxKey, c: MaxKey, d: MaxKey}, chunkArr[2].max);
    assert.eq(chunkArr[0].lastmodEpoch, chunkArr[1].lastmodEpoch);
    assert.eq(chunkArr[1].lastmodEpoch, chunkArr[2].lastmodEpoch);
    assert.neq(oldEpoch, chunkArr[0].lastmodEpoch);
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

    const chunkArr = mongos.getCollection(kConfigChunks).find({ns: kUnrelatedName}).toArray();
    assert.eq(3, chunkArr.length);
    assert.sameMembers(oldChunkArr, chunkArr);

    const tagsArr = mongos.getCollection(kConfigTags).find({ns: kUnrelatedName}).toArray();
    assert.eq(3, tagsArr.length);
    assert.sameMembers(oldTagsArr, tagsArr);
}

jsTestLog('********** SIMPLE TESTS **********');

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

// Should fail because namespace 'db.foo' is not sharded. NOTE: This NamespaceNotSharded error
// is thrown in RefineCollectionShardKeyCommand by 'getShardedCollectionRoutingInfoWithRefresh'.
assert.commandFailedWithCode(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}}),
    ErrorCodes.NamespaceNotSharded);

enableShardingAndShardColl({_id: 1});

// Should fail because shard key is invalid (i.e. bad values).
assert.commandFailedWithCode(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 5}}), ErrorCodes.BadValue);
assert.commandFailedWithCode(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: -1}}), ErrorCodes.BadValue);
assert.commandFailedWithCode(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 'hashed'}}),
    ErrorCodes.BadValue);
assert.commandFailedWithCode(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {aKey: 'hahashed'}}),
    ErrorCodes.BadValue);

// Should fail because shard key is not specified.
assert.commandFailedWithCode(mongos.adminCommand({refineCollectionShardKey: kNsName}), 40414);
assert.commandFailedWithCode(mongos.adminCommand({refineCollectionShardKey: kNsName, key: {}}),
                             ErrorCodes.BadValue);

// Should work because new shard key is already same as current shard key of namespace 'db.foo'.
assert.commandWorked(mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1}}));
dropAndReshardColl({a: 1, b: 1});
assert.commandWorked(mongos.adminCommand({refineCollectionShardKey: kNsName, key: {a: 1, b: 1}}));
dropAndReshardColl({aKey: 'hashed'});
assert.commandWorked(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {aKey: 'hashed'}}));

assert.commandWorked(mongos.getDB(kDbName).dropDatabase());

jsTestLog('********** NAMESPACE VALIDATION TESTS **********');

enableShardingAndShardColl({_id: 1});

// Configure failpoint 'hangRefineCollectionShardKeyAfterRefresh' on staleMongos and run
// refineCollectionShardKey against this mongos in a parallel thread.
assert.commandWorked(staleMongos.adminCommand(
    {configureFailPoint: 'hangRefineCollectionShardKeyAfterRefresh', mode: 'alwaysOn'}));
const awaitShellToTriggerNamespaceNotSharded = startParallelShell(() => {
    assert.commandFailedWithCode(
        db.adminCommand({refineCollectionShardKey: 'db.foo', key: {_id: 1, aKey: 1}}),
        ErrorCodes.NamespaceNotSharded);
}, staleMongos.port);
waitForFailpoint('Hit hangRefineCollectionShardKeyAfterRefresh', 1);

// Drop and re-create namespace 'db.foo' without staleMongos refreshing its metadata.
dropAndRecreateColl({aKey: 1});

// Should fail because namespace 'db.foo' is not sharded. NOTE: This NamespaceNotSharded error
// is thrown in ConfigsvrRefineCollectionShardKeyCommand.
assert.commandWorked(staleMongos.adminCommand(
    {configureFailPoint: 'hangRefineCollectionShardKeyAfterRefresh', mode: 'off'}));
awaitShellToTriggerNamespaceNotSharded();

assert.commandWorked(mongos.adminCommand({shardCollection: kNsName, key: {_id: 1}}));

// Configure failpoint 'hangRefineCollectionShardKeyAfterRefresh' on staleMongos and run
// refineCollectionShardKey against this mongos in a parallel thread.
assert.commandWorked(staleMongos.adminCommand(
    {configureFailPoint: 'hangRefineCollectionShardKeyAfterRefresh', mode: 'alwaysOn'}));
const awaitShellToTriggerStaleEpoch = startParallelShell(() => {
    assert.commandFailedWithCode(
        db.adminCommand({refineCollectionShardKey: 'db.foo', key: {_id: 1, aKey: 1}}),
        ErrorCodes.StaleEpoch);
}, staleMongos.port);
waitForFailpoint('Hit hangRefineCollectionShardKeyAfterRefresh', 2);

// Drop and re-shard namespace 'db.foo' without staleMongos refreshing its metadata.
dropAndReshardColl({_id: 1});

// Should fail because staleMongos has a stale epoch.
assert.commandWorked(staleMongos.adminCommand(
    {configureFailPoint: 'hangRefineCollectionShardKeyAfterRefresh', mode: 'off'}));
awaitShellToTriggerStaleEpoch();

assert.commandWorked(mongos.getDB(kDbName).dropDatabase());

jsTestLog('********** SHARD KEY VALIDATION TESTS **********');

enableShardingAndShardColl({_id: 1});

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

assert.commandFailedWithCode(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}}),
    ErrorCodes.InvalidOptions);

// Should fail because only a partial index exists for new shard key {_id: 1, aKey: 1}.
dropAndReshardColl({_id: 1});
assert.commandWorked(mongos.getCollection(kNsName).createIndex(
    {_id: 1, aKey: 1}, {partialFilterExpression: {aKey: {$gt: 0}}}));

assert.commandFailedWithCode(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}}),
    ErrorCodes.OperationFailed);

// Should fail because only a multikey index exists for new shard key {_id: 1, aKey: 1}.
dropAndReshardColl({_id: 1});
assert.commandWorked(mongos.getCollection(kNsName).createIndex({_id: 1, aKey: 1}));
assert.commandWorked(mongos.getCollection(kNsName).insert({aKey: [1, 2, 3, 4, 5]}));

assert.commandFailedWithCode(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}}),
    ErrorCodes.OperationFailed);

// Should fail because current shard key {a: 1} is unique, new shard key is {a: 1, b: 1}, and an
// index only exists on {a: 1, b: 1, c: 1}.
dropAndReshardCollUnique({a: 1});
assert.commandWorked(mongos.getCollection(kNsName).createIndex({a: 1, b: 1, c: 1}));

assert.commandFailedWithCode(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {a: 1, b: 1}}),
    ErrorCodes.InvalidOptions);

// Should work because current shard key {_id: 1} is not unique, new shard key is {_id: 1, aKey:
// 1}, and an index exists on {_id: 1, aKey: 1, bKey: 1}.
dropAndReshardColl({_id: 1});
assert.commandWorked(mongos.getCollection(kNsName).createIndex({_id: 1, aKey: 1, bKey: 1}));
oldEpoch = mongos.getCollection(kConfigCollections).findOne({_id: kNsName}).lastmodEpoch;

assert.commandWorked(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}}));
validateConfigCollections({_id: 1, aKey: 1}, oldEpoch);
validateConfigChangelog(1);

// Should work because an index with missing or incomplete shard key entries exists for new shard
// key {_id: 1, aKey: 1} and these entries are treated as null values.
dropAndReshardColl({_id: 1});
assert.commandWorked(mongos.getCollection(kNsName).createIndex({_id: 1, aKey: 1}));
assert.commandWorked(mongos.getCollection(kNsName).insert({_id: 12345}));

assert.commandWorked(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}}));

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

// Should work because a 'useful' index exists for new shard key {_id: 1, aKey: 1}.
dropAndReshardColl({_id: 1});
assert.commandWorked(mongos.getCollection(kNsName).createIndex({_id: 1, aKey: 1}));
oldEpoch = mongos.getCollection(kConfigCollections).findOne({_id: kNsName}).lastmodEpoch;

assert.commandWorked(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}}));
validateConfigCollections({_id: 1, aKey: 1}, oldEpoch);
validateConfigChangelog(3);

// Should work because a 'useful' index exists for new shard key {a: 1, b.c: 1}. NOTE: We are
// explicitly verifying that refineCollectionShardKey works with a dotted field.
dropAndReshardColl({a: 1});
assert.commandWorked(mongos.getCollection(kNsName).createIndex({a: 1, 'b.c': 1}));
oldEpoch = mongos.getCollection(kConfigCollections).findOne({_id: kNsName}).lastmodEpoch;

assert.commandWorked(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {a: 1, 'b.c': 1}}));
validateConfigCollections({a: 1, 'b.c': 1}, oldEpoch);
validateConfigChangelog(4);

assert.commandWorked(mongos.getDB(kDbName).dropDatabase());

jsTestLog('********** UNIQUENESS PROPERTY TESTS **********');

enableShardingAndShardColl({_id: 1});
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

enableShardingAndShardColl(oldKeyDoc);
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
const oldChunkArr = mongos.getCollection(kConfigChunks).find({ns: kUnrelatedName}).toArray();
const oldTagsArr = mongos.getCollection(kConfigTags).find({ns: kUnrelatedName}).toArray();
assert.eq(1, oldCollArr.length);
assert.eq(3, oldChunkArr.length);
assert.eq(3, oldTagsArr.length);

assert.commandWorked(mongos.adminCommand({refineCollectionShardKey: kNsName, key: newKeyDoc}));
validateUnrelatedCollAfterRefine(oldCollArr, oldChunkArr, oldTagsArr);

// Verify that all shards containing chunks in the namespace 'db.foo' eventually refresh (i.e. the
// secondary shard will not refresh because it does not contain any chunks in 'db.foo'). NOTE: This
// will only succeed in a linear jstest without failovers.
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
assert.soon(() => oldSecondaryEpoch ===
                st.shard1.adminCommand({getShardVersion: kNsName, fullMetadata: true})
                    .metadata.shardVersionEpoch.toString());

st.stop();
})();
