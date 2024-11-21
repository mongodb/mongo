import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

const kConfigCollections = 'config.collections';
const kConfigTags = 'config.tags';
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

export function dropAndReshardColl(mongos, dbName, collName, keyDoc) {
    assert.commandWorked(mongos.getDB(dbName).runCommand({drop: collName}));
    assert.commandWorked(
        mongos.adminCommand({shardCollection: dbName + "." + collName, key: keyDoc}));
}

export function dropAndReshardCollUnique(mongos, dbName, collName, keyDoc) {
    assert.commandWorked(mongos.getDB(dbName).runCommand({drop: collName}));
    assert.commandWorked(
        mongos.adminCommand({shardCollection: dbName + "." + collName, key: keyDoc, unique: true}));
}

export function validateConfigCollections(mongos, nsName, keyDoc, oldEpoch) {
    const collArr = mongos.getCollection(kConfigCollections).find({_id: nsName}).toArray();
    assert.eq(1, collArr.length);
    assert.eq(keyDoc, collArr[0].key);
    assert.neq(oldEpoch, collArr[0].lastmodEpoch);
}

export function validateConfigCollectionsUnique(mongos, nsName, unique) {
    const collArr = mongos.getCollection(kConfigCollections).find({_id: nsName}).toArray();
    assert.eq(1, collArr.length);
    assert.eq(unique, collArr[0].unique);
}

export function validateSplitAfterRefine(mongos, nsName) {
    // The full shard key is required when manually specifying bounds.
    assert.commandFailed(mongos.adminCommand({split: nsName, middle: {a: 0, b: 0}}));
    assert.commandWorked(mongos.adminCommand({split: nsName, middle: {a: 0, b: 0, c: 0, d: 0}}));
}

export function validateMoveAfterRefine(mongos, nsName, secondaryShard) {
    // The full shard key is required when manually specifying bounds.
    assert.commandFailedWithCode(
        mongos.adminCommand({moveChunk: nsName, find: {a: 5, b: 5}, to: secondaryShard}), 656450);
    assert.commandWorked(mongos.adminCommand(
        {moveChunk: nsName, find: {a: 5, b: 5, c: 5, d: 5}, to: secondaryShard}));
}

export function validateMergeAfterRefine(mongos, nsName) {
    assert.commandWorked(mongos.adminCommand({split: nsName, middle: {a: 0, b: 0, c: 0, d: 0}}));
    assert.commandWorked(
        mongos.adminCommand({split: nsName, middle: {a: 10, b: 10, c: 10, d: 10}}));

    // The full shard key is required when manually specifying bounds.
    assert.commandFailed(mongos.adminCommand(
        {mergeChunks: nsName, bounds: [{a: MinKey, b: MinKey}, {a: MaxKey, b: MaxKey}]}));
    assert.commandWorked(mongos.adminCommand({
        mergeChunks: nsName,
        bounds: [
            {a: MinKey, b: MinKey, c: MinKey, d: MinKey},
            {a: MaxKey, b: MaxKey, c: MaxKey, d: MaxKey}
        ]
    }));
}

export function setupConfigChunksBeforeRefine(mongos, nsName) {
    // Ensure there exist 2 chunks that are not the global max chunk to properly verify the
    // correctness of the multi-update in refineCollectionShardKey.
    assert.commandWorked(mongos.adminCommand({split: nsName, middle: {a: 0, b: 0}}));
    assert.commandWorked(mongos.adminCommand({split: nsName, middle: {a: 5, b: 5}}));

    return findChunksUtil.findOneChunkByNs(mongos.getDB('config'), nsName).lastmodEpoch;
}

export function validateConfigChunksAfterRefine(mongos, nsName, oldEpoch) {
    const chunkArr =
        findChunksUtil.findChunksByNs(mongos.getDB('config'), nsName).sort({min: 1}).toArray();
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

export function setupConfigTagsBeforeRefine(mongos, nsName, primaryShard) {
    // Ensure there exist 2 tags that are not the global max tag to properly verify the
    // correctness of the multi-update in refineCollectionShardKey.
    assert.commandWorked(mongos.adminCommand({addShardToZone: primaryShard, zone: 'zone_1'}));
    assert.commandWorked(mongos.adminCommand({addShardToZone: primaryShard, zone: 'zone_2'}));
    assert.commandWorked(mongos.adminCommand({addShardToZone: primaryShard, zone: 'zone_3'}));
    assert.commandWorked(mongos.adminCommand({
        updateZoneKeyRange: nsName,
        min: {a: MinKey, b: MinKey},
        max: {a: 0, b: 0},
        zone: 'zone_1'
    }));
    assert.commandWorked(mongos.adminCommand(
        {updateZoneKeyRange: nsName, min: {a: 0, b: 0}, max: {a: 5, b: 5}, zone: 'zone_2'}));
    assert.commandWorked(mongos.adminCommand({
        updateZoneKeyRange: nsName,
        min: {a: 5, b: 5},
        max: {a: MaxKey, b: MaxKey},
        zone: 'zone_3'
    }));
}

export function validateConfigTagsAfterRefine(mongos, nsName) {
    const tagsArr = mongos.getCollection(kConfigTags).find({ns: nsName}).sort({min: 1}).toArray();
    assert.eq(3, tagsArr.length);
    assert.eq({a: MinKey, b: MinKey, c: MinKey, d: MinKey}, tagsArr[0].min);
    assert.eq({a: 0, b: 0, c: MinKey, d: MinKey}, tagsArr[0].max);
    assert.eq({a: 0, b: 0, c: MinKey, d: MinKey}, tagsArr[1].min);
    assert.eq({a: 5, b: 5, c: MinKey, d: MinKey}, tagsArr[1].max);
    assert.eq({a: 5, b: 5, c: MinKey, d: MinKey}, tagsArr[2].min);
    assert.eq({a: MaxKey, b: MaxKey, c: MaxKey, d: MaxKey}, tagsArr[2].max);
}

export function validateUnrelatedCollAfterRefine(
    mongos, unrelatedName, oldCollArr, oldChunkArr, oldTagsArr) {
    const collArr = mongos.getCollection(kConfigCollections).find({_id: unrelatedName}).toArray();
    assert.eq(1, collArr.length);
    assert.sameMembers(oldCollArr, collArr);

    const chunkArr = findChunksUtil.findChunksByNs(mongos.getDB('config'), unrelatedName).toArray();
    assert.eq(3, chunkArr.length);
    assert.sameMembers(oldChunkArr, chunkArr);

    const tagsArr = mongos.getCollection(kConfigTags).find({ns: unrelatedName}).toArray();
    assert.eq(3, tagsArr.length);
    assert.sameMembers(oldTagsArr, tagsArr);
}

export function simpleValidationTests(mongosConn, dbName) {
    jsTestLog('********** SIMPLE TESTS **********');
    const collName = "simpleValidationTests";
    const kNsName = dbName + "." + collName;

    // Should fail because arguments 'refineCollectionShardKey' and 'key' are invalid types.
    assert.commandFailedWithCode(
        mongosConn.adminCommand({refineCollectionShardKey: {_id: 1}, key: {_id: 1, aKey: 1}}),
        ErrorCodes.TypeMismatch);
    assert.commandFailedWithCode(
        mongosConn.adminCommand({refineCollectionShardKey: kNsName, key: 'blah'}),
        ErrorCodes.TypeMismatch);

    // Should fail because refineCollectionShardKey may only be run against the admin database.
    assert.commandFailedWithCode(mongosConn.getDB(dbName).runCommand(
                                     {refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}}),
                                 ErrorCodes.Unauthorized);

    // Should fail because namespace does not exist.
    assert.commandFailedWithCode(
        mongosConn.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}}),
        ErrorCodes.NamespaceNotFound);

    assert.commandWorked(mongosConn.getCollection(kNsName).insert({aKey: 1}));

    // Should fail because namespace is not sharded.
    assert.commandFailedWithCode(
        mongosConn.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}}),
        ErrorCodes.NamespaceNotSharded);

    assert.commandFailedWithCode(
        mongosConn.adminCommand(
            {refineCollectionShardKey: "config.collections", key: {_id: 1, aKey: 1}}),
        ErrorCodes.NamespaceNotSharded);

    assert.commandWorked(mongosConn.adminCommand({shardCollection: kNsName, key: {_id: 1}}));

    // Should fail because shard key is invalid (i.e. bad values).
    assert.commandFailedWithCode(
        mongosConn.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 5}}),
        ErrorCodes.BadValue);
    assert.commandFailedWithCode(
        mongosConn.adminCommand({refineCollectionShardKey: kNsName, key: {_id: -1}}),
        ErrorCodes.BadValue);
    assert.commandFailedWithCode(
        mongosConn.adminCommand(
            {refineCollectionShardKey: kNsName, key: {_id: 'hashed', aKey: 'hashed'}}),
        ErrorCodes.BadValue);
    assert.commandFailedWithCode(
        mongosConn.adminCommand({refineCollectionShardKey: kNsName, key: {aKey: 'hahashed'}}),
        ErrorCodes.BadValue);

    // Should fail because shard key is not specified.
    assert.commandFailedWithCode(mongosConn.adminCommand({refineCollectionShardKey: kNsName}),
                                 ErrorCodes.IDLFailedToParse);
    assert.commandFailedWithCode(
        mongosConn.adminCommand({refineCollectionShardKey: kNsName, key: {}}), ErrorCodes.BadValue);

    // Should work because new shard key is already same as current shard key of namespace.
    assert.commandWorked(
        mongosConn.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1}}));
    dropAndReshardColl(mongosConn, dbName, collName, {a: 1, b: 1});
    assert.commandWorked(
        mongosConn.adminCommand({refineCollectionShardKey: kNsName, key: {a: 1, b: 1}}));
    dropAndReshardColl(mongosConn, dbName, collName, {aKey: 'hashed'});
    assert.commandWorked(
        mongosConn.adminCommand({refineCollectionShardKey: kNsName, key: {aKey: 'hashed'}}));
    dropAndReshardColl(mongosConn, dbName, collName, {_id: 1, aKey: 'hashed'});
    assert.commandWorked(mongosConn.adminCommand(
        {refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 'hashed'}}));

    // Make sure split is correctly disabled for unsplittable collection
    (() => {
        if (FeatureFlagUtil.isPresentAndEnabled(mongosConn,
                                                "TrackUnshardedCollectionsUponCreation")) {
            jsTest.log(
                "Make sure refine shard key for unsplittable collection is correctly disabled");
            const kCollNameUnsplittable = "unsplittable_bar";
            const kNsNameUnsplittable = dbName + "." + kCollNameUnsplittable;
            assert.commandWorked(mongosConn.getDB(dbName).runCommand(
                {createUnsplittableCollection: kCollNameUnsplittable}));
            assert.commandFailedWithCode(
                mongosConn.adminCommand(
                    {refineCollectionShardKey: kNsNameUnsplittable, key: {a: 1, b: 1}}),
                ErrorCodes.NamespaceNotSharded);
        }
    })();

    assert.commandWorked(mongosConn.getDB(dbName).dropDatabase());
}

export function shardKeyValidationTests(mongosConn, dbName) {
    jsTestLog('********** SHARD KEY VALIDATION TESTS **********');
    const collName = "shardKeyValidationTests";
    const kNsName = dbName + "." + collName;

    assert.commandWorked(mongosConn.adminCommand({shardCollection: kNsName, key: {_id: 1}}));

    // Should fail because new shard key {aKey: 1} does not extend current shard key {_id: 1} of
    // namespace.
    assert.commandFailedWithCode(
        mongosConn.adminCommand({refineCollectionShardKey: kNsName, key: {aKey: 1}}),
        ErrorCodes.InvalidOptions);

    // Should fail because no index exists for new shard key {_id: 1, aKey: 1}.
    assert.commandFailedWithCode(
        mongosConn.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}}),
        ErrorCodes.InvalidOptions);

    // Should fail because only a sparse index exists for new shard key {_id: 1, aKey: 1}.
    dropAndReshardColl(mongosConn, dbName, collName, {_id: 1});
    assert.commandWorked(
        mongosConn.getCollection(kNsName).createIndex({_id: 1, aKey: 1}, {sparse: true}));

    var result =
        mongosConn.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}});
    assert.commandFailedWithCode(result, ErrorCodes.InvalidOptions);
    assert(result.errmsg.includes("Index key is sparse."));

    // Should fail because index has a non-simple collation.
    dropAndReshardColl(mongosConn, dbName, collName, {aKey: 1});
    assert.commandWorked(mongosConn.getCollection(kNsName).createIndex({aKey: 1, bKey: 1}, {
        collation: {
            locale: "en",
        }
    }));
    result = mongosConn.adminCommand({refineCollectionShardKey: kNsName, key: {aKey: 1, bKey: 1}});
    assert.commandFailedWithCode(result, ErrorCodes.InvalidOptions);
    assert(result.errmsg.includes("Index has a non-simple collation."));

    // Should fail because only a partial index exists for new shard key {_id: 1, aKey: 1}.
    dropAndReshardColl(mongosConn, dbName, collName, {_id: 1});
    assert.commandWorked(mongosConn.getCollection(kNsName).createIndex(
        {_id: 1, aKey: 1}, {partialFilterExpression: {aKey: {$gt: 0}}}));

    result = mongosConn.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}});
    assert.commandFailedWithCode(result, ErrorCodes.InvalidOptions);
    assert(result.errmsg.includes("Index key is partial."));

    // Should fail because only a multikey index exists for new shard key {_id: 1, aKey: 1}.
    dropAndReshardColl(mongosConn, dbName, collName, {_id: 1});
    assert.commandWorked(mongosConn.getCollection(kNsName).createIndex({_id: 1, aKey: 1}));
    assert.commandWorked(mongosConn.getCollection(kNsName).insert({aKey: [1, 2, 3, 4, 5]}));

    result = mongosConn.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}});
    assert.commandFailedWithCode(result, ErrorCodes.InvalidOptions);
    assert(result.errmsg.includes("Index key is multikey."));

    // Should fail because current shard key {a: 1} is unique, new shard key is {a: 1, b: 1}, and an
    // index only exists on {a: 1, b: 1, c: 1}.
    dropAndReshardCollUnique(mongosConn, dbName, collName, {a: 1});
    assert.commandWorked(mongosConn.getCollection(kNsName).createIndex({a: 1, b: 1, c: 1}));

    mongosConn.adminCommand({refineCollectionShardKey: kNsName, key: {a: 1, b: 1}});
    assert.commandFailedWithCode(result, ErrorCodes.InvalidOptions);

    // Should work because current shard key {_id: 1} is not unique, new shard key is {_id: 1, aKey:
    // 1}, and an index exists on {_id: 1, aKey: 1, bKey: 1}.
    dropAndReshardColl(mongosConn, dbName, collName, {_id: 1});
    assert.commandWorked(mongosConn.getCollection(kNsName).createIndex({_id: 1, aKey: 1, bKey: 1}));
    let oldEpoch =
        mongosConn.getCollection(kConfigCollections).findOne({_id: kNsName}).lastmodEpoch;

    assert.commandWorked(
        mongosConn.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}}));
    validateConfigCollections(mongosConn, kNsName, {_id: 1, aKey: 1}, oldEpoch);

    // Should work because an index with missing or incomplete shard key entries exists for new
    // shard key {_id: 1, aKey: 1} and these entries are treated as null values.
    dropAndReshardColl(mongosConn, dbName, collName, {_id: 1});
    assert.commandWorked(mongosConn.getCollection(kNsName).createIndex({_id: 1, aKey: 1}));
    assert.commandWorked(mongosConn.getCollection(kNsName).insert({_id: 12345}));

    assert.commandWorked(
        mongosConn.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}}));

    // Should work because an index with missing or incomplete shard key entries exists for new
    // shard key {_id: "hashed", aKey: 1} and these entries are treated as null values.
    dropAndReshardColl(mongosConn, dbName, collName, {_id: "hashed"});
    assert.commandWorked(mongosConn.getCollection(kNsName).createIndex({_id: "hashed", aKey: 1}));
    assert.commandWorked(mongosConn.getCollection(kNsName).insert({_id: 12345}));

    assert.commandWorked(mongosConn.adminCommand(
        {refineCollectionShardKey: kNsName, key: {_id: "hashed", aKey: 1}}));

    // Should work because an index with missing or incomplete shard key entries exists for new
    // shard key {_id: 1, aKey: "hashed"} and these entries are treated as null values.
    dropAndReshardColl(mongosConn, dbName, collName, {_id: 1});
    assert.commandWorked(mongosConn.getCollection(kNsName).createIndex({_id: 1, aKey: "hashed"}));
    assert.commandWorked(mongosConn.getCollection(kNsName).insert({_id: 12345}));

    assert.commandWorked(mongosConn.adminCommand(
        {refineCollectionShardKey: kNsName, key: {_id: 1, aKey: "hashed"}}));

    // Should fail because new shard key {aKey: 1} is not a prefix of current shard key {_id: 1,
    // aKey: 1}.
    dropAndReshardColl(mongosConn, dbName, collName, {_id: 1, aKey: 1});
    assert.commandWorked(mongosConn.getCollection(kNsName).createIndex({aKey: 1}));

    assert.commandFailedWithCode(
        mongosConn.adminCommand({refineCollectionShardKey: kNsName, key: {aKey: 1}}),
        ErrorCodes.InvalidOptions);

    // Should fail because new shard key {aKey: 1, _id: 1} is not a prefix of current shard key
    // {_id: 1, aKey: 1}.
    dropAndReshardColl(mongosConn, dbName, collName, {_id: 1, aKey: 1});
    assert.commandWorked(mongosConn.getCollection(kNsName).createIndex({aKey: 1, _id: 1}));

    assert.commandFailedWithCode(
        mongosConn.adminCommand({refineCollectionShardKey: kNsName, key: {aKey: 1, _id: 1}}),
        ErrorCodes.InvalidOptions);

    // Should fail because new shard key {aKey: 1, _id: 1, bKey: 1} is not a prefix of current shard
    // key {_id: 1, aKey: 1}.
    dropAndReshardColl(mongosConn, dbName, collName, {_id: 1, aKey: 1});
    assert.commandWorked(mongosConn.getCollection(kNsName).createIndex({aKey: 1, _id: 1, bKey: 1}));

    assert.commandFailedWithCode(
        mongosConn.adminCommand(
            {refineCollectionShardKey: kNsName, key: {aKey: 1, _id: 1, bKey: 1}}),
        ErrorCodes.InvalidOptions);

    // Should fail because new shard key {aKey: 1, bKey: 1} is not a prefix of current shard key
    // {_id: 1}.
    dropAndReshardColl(mongosConn, dbName, collName, {_id: 1});
    assert.commandWorked(mongosConn.getCollection(kNsName).createIndex({aKey: 1, bKey: 1}));

    assert.commandFailedWithCode(
        mongosConn.adminCommand({refineCollectionShardKey: kNsName, key: {aKey: 1, bKey: 1}}),
        ErrorCodes.InvalidOptions);

    // Should fail because index key is sparse and index has non-simple collation.
    dropAndReshardColl(mongosConn, dbName, collName, {_id: 1});
    assert.commandWorked(mongosConn.getCollection(kNsName).createIndex({_id: 1, aKey: 1}, {
        sparse: true,
        collation: {
            locale: "en",
        }
    }));
    result = mongosConn.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}});
    assert.commandFailedWithCode(result, ErrorCodes.InvalidOptions);
    assert(result.errmsg.includes("Index key is sparse.") &&
           result.errmsg.includes("Index has a non-simple collation."));

    // Should fail because index key is multikey and is partial.
    dropAndReshardColl(mongosConn, dbName, collName, {_id: 1});
    assert.commandWorked(mongosConn.getCollection(kNsName).createIndex(
        {_id: 1, aKey: 1}, {name: "index_1_part", partialFilterExpression: {aKey: {$gt: 0}}}));
    assert.commandWorked(
        mongosConn.getCollection(kNsName).createIndex({_id: 1, aKey: 1}, {name: "index_2"}));
    assert.commandWorked(mongosConn.getCollection(kNsName).insert({aKey: [1, 2, 3, 4, 5]}));

    result = mongosConn.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}});
    assert.commandFailedWithCode(result, ErrorCodes.InvalidOptions);
    assert(result.errmsg.includes("Index key is multikey.") &&
           result.errmsg.includes("Index key is partial."));

    // Should fail because both indexes have keys that are incompatible: partial; sparse
    dropAndReshardColl(mongosConn, dbName, collName, {_id: 1});
    assert.commandWorked(mongosConn.getCollection(kNsName).createIndex(
        {_id: 1, aKey: 1}, {name: "index_1_part", partialFilterExpression: {aKey: {$gt: 0}}}));
    assert.commandWorked(mongosConn.getCollection(kNsName).createIndex(
        {_id: 1, aKey: 1}, {name: "index_2_sparse", sparse: true}));
    result = mongosConn.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}});
    assert.commandFailedWithCode(result, ErrorCodes.InvalidOptions);
    assert(result.errmsg.includes("Index key is partial.") &&
           result.errmsg.includes("Index key is sparse."));

    // Should work because a 'useful' index exists for new shard key {_id: 1, aKey: 1}.
    dropAndReshardColl(mongosConn, dbName, collName, {_id: 1});
    assert.commandWorked(mongosConn.getCollection(kNsName).createIndex({_id: 1, aKey: 1}));
    oldEpoch = mongosConn.getCollection(kConfigCollections).findOne({_id: kNsName}).lastmodEpoch;

    assert.commandWorked(
        mongosConn.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}}));
    validateConfigCollections(mongosConn, kNsName, {_id: 1, aKey: 1}, oldEpoch);

    // Should work because a 'useful' index exists for new shard key {a: 1, b.c: 1}. NOTE: We are
    // explicitly verifying that refineCollectionShardKey works with a dotted field.
    dropAndReshardColl(mongosConn, dbName, collName, {a: 1});
    assert.commandWorked(mongosConn.adminCommand({split: kNsName, middle: {a: 0}}));
    assert.commandWorked(mongosConn.getCollection(kNsName).createIndex({a: 1, 'b.c': 1}));
    oldEpoch = mongosConn.getCollection(kConfigCollections).findOne({_id: kNsName}).lastmodEpoch;

    assert.commandWorked(
        mongosConn.adminCommand({refineCollectionShardKey: kNsName, key: {a: 1, 'b.c': 1}}));
    assert.commandWorked(mongosConn.adminCommand({split: kNsName, middle: {a: 0, 'b.c': 0}}));
    validateConfigCollections(mongosConn, kNsName, {a: 1, 'b.c': 1}, oldEpoch);

    // Refining a shard key with a dotted field to include more dotted fields should work.
    dropAndReshardColl(mongosConn, dbName, collName, {a: 1, 'b.c': 1});
    assert.commandWorked(mongosConn.adminCommand({split: kNsName, middle: {a: 0, 'b.c': 0}}));
    assert.commandWorked(
        mongosConn.getCollection(kNsName).createIndex({a: 1, 'b.c': 1, d: 1, 'e.f.g': 1, h: 1}));
    oldEpoch = mongosConn.getCollection(kConfigCollections).findOne({_id: kNsName}).lastmodEpoch;

    assert.commandWorked(mongosConn.adminCommand(
        {refineCollectionShardKey: kNsName, key: {a: 1, 'b.c': 1, d: 1, 'e.f.g': 1, h: 1}}));
    assert.commandWorked(mongosConn.adminCommand(
        {split: kNsName, middle: {a: 0, 'b.c': 0, d: 0, 'e.f.g': 0, h: 0}}));
    validateConfigCollections(
        mongosConn, kNsName, {a: 1, 'b.c': 1, d: 1, 'e.f.g': 1, h: 1}, oldEpoch);

    // Refining a shard key with a dotted field to include a non-dotted field should work.
    dropAndReshardColl(mongosConn, dbName, collName, {'a.b': 1});
    assert.commandWorked(mongosConn.adminCommand({split: kNsName, middle: {'a.b': 0}}));
    assert.commandWorked(mongosConn.getCollection(kNsName).createIndex({'a.b': 1, c: 1}));
    oldEpoch = mongosConn.getCollection(kConfigCollections).findOne({_id: kNsName}).lastmodEpoch;

    assert.commandWorked(
        mongosConn.adminCommand({refineCollectionShardKey: kNsName, key: {'a.b': 1, c: 1}}));
    assert.commandWorked(mongosConn.adminCommand({split: kNsName, middle: {'a.b': 0, c: 0}}));
    validateConfigCollections(mongosConn, kNsName, {'a.b': 1, c: 1}, oldEpoch);

    assert.commandWorked(mongosConn.getDB(dbName).dropDatabase());
}

export function uniquePropertyTests(mongosConn, dbName) {
    jsTestLog('********** UNIQUENESS PROPERTY TESTS **********');
    const collName = "uniquenessPropertyTests";
    const kNsName = dbName + "." + collName;

    assert.commandWorked(mongosConn.adminCommand({shardCollection: kNsName, key: {_id: 1}}));
    assert.commandWorked(mongosConn.getCollection(kNsName).createIndex({_id: 1, aKey: 1}));

    // Verify that refineCollectionShardKey cannot modify a unique=false sharded collection.
    assert.commandWorked(
        mongosConn.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}}));
    validateConfigCollectionsUnique(mongosConn, kNsName, false);

    // Verify that refineCollectionShardKey cannot modify a unique=true sharded collection.
    dropAndReshardCollUnique(mongosConn, dbName, collName, {_id: 1});
    assert.commandWorked(mongosConn.getCollection(kNsName).createIndex({_id: 1, aKey: 1}));

    assert.commandWorked(
        mongosConn.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}}));
    validateConfigCollectionsUnique(mongosConn, kNsName, true);

    // Verify that enforceUniquenessCheck: false allows non-unique indexes.
    assert.commandWorked(mongosConn.getDB(dbName).runCommand({drop: collName}));
    assert.commandWorked(mongosConn.getCollection(kNsName).createIndex({a: 1, b: 1}));
    assert.commandWorked(mongosConn.adminCommand(
        {shardCollection: kNsName, key: {a: 1}, unique: true, enforceUniquenessCheck: false}));
    assert.commandWorked(mongosConn.adminCommand(
        {refineCollectionShardKey: kNsName, key: {a: 1, b: 1}, enforceUniquenessCheck: false}));
    validateConfigCollectionsUnique(mongosConn, kNsName, true);

    assert.commandWorked(mongosConn.getDB(dbName).dropDatabase());
}

export function integrationTests(mongosConn, dbName, primaryShard, secondaryShard) {
    jsTestLog('********** COMMON INTEGRATION TESTS **********');
    const collName = "integrationTests";
    const kNsName = dbName + "." + collName;

    // Split chunk operations before and after refineCollectionShardKey should work as expected.
    dropAndReshardColl(mongosConn, dbName, collName, oldKeyDoc);
    assert.commandWorked(mongosConn.getCollection(kNsName).createIndex(newKeyDoc));

    assert.commandWorked(
        mongosConn.adminCommand({refineCollectionShardKey: kNsName, key: newKeyDoc}));
    validateSplitAfterRefine(mongosConn, kNsName);

    if (secondaryShard) {
        // Move chunk operations before and after refineCollectionShardKey should work as expected.
        dropAndReshardColl(mongosConn, dbName, collName, oldKeyDoc);
        assert.commandWorked(mongosConn.getCollection(kNsName).createIndex(newKeyDoc));
        assert.commandWorked(mongosConn.adminCommand({split: kNsName, middle: {a: 0, b: 0}}));
        assert.commandWorked(mongosConn.adminCommand({split: kNsName, middle: {a: 10, b: 10}}));

        assert.commandWorked(
            mongosConn.adminCommand({refineCollectionShardKey: kNsName, key: newKeyDoc}));
        validateMoveAfterRefine(mongosConn, kNsName, secondaryShard);
    }

    // Merge chunk operations before and after refineCollectionShardKey should work as expected.
    dropAndReshardColl(mongosConn, dbName, collName, oldKeyDoc);
    assert.commandWorked(mongosConn.getCollection(kNsName).createIndex(newKeyDoc));

    assert.commandWorked(
        mongosConn.adminCommand({refineCollectionShardKey: kNsName, key: newKeyDoc}));
    validateMergeAfterRefine(mongosConn, kNsName);

    // The config.chunks collection before and after refineCollectionShardKey should be as expected.
    dropAndReshardColl(mongosConn, dbName, collName, oldKeyDoc);
    assert.commandWorked(mongosConn.getCollection(kNsName).createIndex(newKeyDoc));

    let oldEpoch = setupConfigChunksBeforeRefine(mongosConn, kNsName);
    assert.commandWorked(
        mongosConn.adminCommand({refineCollectionShardKey: kNsName, key: newKeyDoc}));
    validateConfigChunksAfterRefine(mongosConn, kNsName, oldEpoch);

    // The config.tags collection before and after refineCollectionShardKey should be as expected.
    dropAndReshardColl(mongosConn, dbName, collName, oldKeyDoc);
    assert.commandWorked(mongosConn.getCollection(kNsName).createIndex(newKeyDoc));

    setupConfigTagsBeforeRefine(mongosConn, kNsName, primaryShard);
    assert.commandWorked(
        mongosConn.adminCommand({refineCollectionShardKey: kNsName, key: newKeyDoc}));
    validateConfigTagsAfterRefine(mongosConn, kNsName);

    assert.commandWorked(mongosConn.getDB(dbName).dropDatabase());
}
