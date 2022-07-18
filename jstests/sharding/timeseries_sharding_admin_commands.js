/**
 * Test sharding admin commands on sharded time-series collection.
 *
 * @tags: [
 *   requires_fcv_51
 * ]
 */

(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

// Connections.
const mongo = new ShardingTest({shards: 2, rs: {nodes: 3}});

// Sanity checks.
if (!TimeseriesTest.shardedtimeseriesCollectionsEnabled(mongo.shard0)) {
    jsTestLog("Skipping test because the sharded time-series collection feature flag is disabled");
    mongo.stop();
    return;
}

const dbName = 'testDB';
const collName = 'testColl';
const timeField = 'time';
const metaField = 'meta';
const viewNss = `${dbName}.${collName}`;
const bucketNss = `${dbName}.system.buckets.${collName}`;
const controlTimeField = `control.min.${timeField}`;
const numDocsInserted = 20;
const zone = 'Z';
assert.commandWorked(mongo.s0.adminCommand({enableSharding: dbName}));
assert.commandWorked(mongo.s0.adminCommand({addShardToZone: mongo.shard0.shardName, zone: zone}));

function createTimeSeriesColl({index, shardKey}) {
    const db = mongo.s0.getDB(dbName);
    assert.commandWorked(
        db.createCollection(collName, {timeseries: {timeField: timeField, metaField: metaField}}));
    assert.commandWorked(db[collName].createIndex(index));
    for (let i = 0; i < numDocsInserted; i++) {
        assert.commandWorked(db[collName].insert({[metaField]: i, [timeField]: ISODate()}));
    }
    if (shardKey) {
        assert.commandWorked(mongo.s0.adminCommand({
            shardCollection: viewNss,
            key: shardKey,
            timeseries: {timeField: timeField, metaField: metaField}
        }));
    }
}

function dropTimeSeriesColl() {
    const db = mongo.s0.getDB(dbName);
    const coll = db.getCollection(collName);
    assert(coll.drop());
}

// Check the zone range against the extended range saved in config.tags collection.
function assertRangeMatch(savedRange, paramRange) {
    // Non-extended range key must match.
    Object.keys(paramRange).forEach(key => {
        assert.docEq(savedRange[key], paramRange[key]);
    });
    // Extended range key must be MinKey.
    Object.keys(savedRange).forEach(key => {
        if (!paramRange.hasOwnProperty(key)) {
            assert.docEq(savedRange[key], MinKey);
        }
    });
}

// Check updateZoneKeyRange with range other than MinKey -> MinKey on the time field prevents
// sharding. The last successful call will shard the collection.
(function checkUpdateZoneKeyRangeCommandBeforeSharding() {
    function check(min, max, success) {
        createTimeSeriesColl({index: {[metaField]: 1, [timeField]: 1}});
        assert.commandWorked(
            mongo.s0.adminCommand({updateZoneKeyRange: bucketNss, min: min, max: max, zone: zone}));
        const tag = mongo.s0.getDB('config').tags.findOne({ns: bucketNss});
        assertRangeMatch(tag.min, min);
        assertRangeMatch(tag.max, max);
        const result = mongo.s0.adminCommand({
            shardCollection: viewNss,
            key: {[metaField]: 1, [timeField]: 1},
            timeseries: {timeField: timeField, metaField: metaField}
        });
        if (success) {
            assert.commandWorked(result);
        } else {
            assert.commandFailedWithCode(result, ErrorCodes.InvalidOptions);
        }
        assert.commandWorked(
            mongo.s0.adminCommand({updateZoneKeyRange: bucketNss, min: min, max: max, zone: null}));
        assert.eq(0, mongo.s0.getDB('config').tags.find({ns: bucketNss}).count());
        dropTimeSeriesColl();
    }
    check(
        {[metaField]: 1, [controlTimeField]: 1}, {[metaField]: 10, [controlTimeField]: 10}, false);
    check({[metaField]: 1, [controlTimeField]: 1},
          {[metaField]: 10, [controlTimeField]: MaxKey},
          false);
    check({[metaField]: 1, [controlTimeField]: MinKey},
          {[metaField]: 10, [controlTimeField]: 10},
          false);
    check({[metaField]: 1, [controlTimeField]: MinKey},
          {[metaField]: 10, [controlTimeField]: MaxKey},
          false);
    check({[metaField]: 1, [controlTimeField]: MinKey},
          {[metaField]: 10, [controlTimeField]: MinKey},
          true);
    check({[metaField]: 1}, {[metaField]: 10}, false);
})();

// Check updateZoneKeyRange rejects range other than MinKey -> MinKey on the time field after
// sharding.
(function checkUpdateZoneKeyRangeAfterSharding() {
    function check(min, max, success) {
        createTimeSeriesColl(
            {index: {[metaField]: 1, [timeField]: 1}, shardKey: {[metaField]: 1, [timeField]: 1}});
        const result =
            mongo.s0.adminCommand({updateZoneKeyRange: bucketNss, min: min, max: max, zone: zone});
        if (success) {
            assert.commandWorked(result);
            const tag = mongo.s0.getDB('config').tags.findOne({ns: bucketNss});
            assertRangeMatch(tag.min, min);
            assertRangeMatch(tag.max, max);
            assert.commandWorked(mongo.s0.adminCommand(
                {updateZoneKeyRange: bucketNss, min: min, max: max, zone: null}));
            assert.eq(0, mongo.s0.getDB('config').tags.find({ns: bucketNss}).count());
        } else {
            assert.commandFailedWithCode(result, ErrorCodes.InvalidOptions);
        }
        dropTimeSeriesColl();
    }
    check(
        {[metaField]: 1, [controlTimeField]: 1}, {[metaField]: 10, [controlTimeField]: 10}, false);
    check({[metaField]: 1, [controlTimeField]: 1},
          {[metaField]: 10, [controlTimeField]: MaxKey},
          false);
    check({[metaField]: 1, [controlTimeField]: MinKey},
          {[metaField]: 10, [controlTimeField]: 10},
          false);
    check({[metaField]: 1, [controlTimeField]: MinKey},
          {[metaField]: 10, [controlTimeField]: MaxKey},
          false);
    check({[metaField]: 1, [controlTimeField]: MinKey},
          {[metaField]: 10, [controlTimeField]: MinKey},
          true);
    check({[metaField]: 1}, {[metaField]: 10}, true);
})();

// Check shardingState commands returns the expected collection info about buckets & view nss.
(function checkShardingStateCommand() {
    createTimeSeriesColl(
        {index: {[metaField]: 1, [timeField]: 1}, shardKey: {[metaField]: 1, [timeField]: 1}});
    const shardingStateRes = mongo.getPrimaryShard(dbName).adminCommand({shardingState: 1});
    const shardingStateColls = shardingStateRes.versions;
    const bucketNssIsSharded = (bucketNss in shardingStateColls &&
                                timestampCmp(shardingStateColls[bucketNss], Timestamp(0, 0)) !== 0);
    const viewNssIsSharded = (viewNss in shardingStateColls &&
                              timestampCmp(shardingStateColls[viewNss], Timestamp(0, 0)) !== 0);
    assert(bucketNssIsSharded && !viewNssIsSharded);
    dropTimeSeriesColl();
})();

// Check reshardCollection commands are disabled for time-series collection.
(function checkReshardCollectionCommand() {
    createTimeSeriesColl({index: {[metaField]: 1, [timeField]: 1}, shardKey: {[metaField]: 1}});
    // Command is not supported on the time-series view namespace.
    assert.commandFailedWithCode(
        mongo.s0.adminCommand(
            {reshardCollection: viewNss, key: {[metaField]: 1, [controlTimeField]: 1}}),
        [
            ErrorCodes.NotImplemented,
            ErrorCodes.NamespaceNotSharded /* TODO SERVER-67929 Remove this error code */
        ]);
    assert.commandFailedWithCode(
        mongo.s0.adminCommand(
            {reshardCollection: bucketNss, key: {[metaField]: 1, [controlTimeField]: 1}}),
        ErrorCodes.NotImplemented);
    dropTimeSeriesColl();
})();

// Check checkShardingIndex works for the correct key pattern on the bucket namespace,
// but not on the view namespace or in correct key pattern.
(function checkCheckShardingIndexCommand() {
    createTimeSeriesColl(
        {index: {[metaField]: 1, [timeField]: 1}, shardKey: {[metaField]: 1, [timeField]: 1}});
    const primaryShard = mongo.getPrimaryShard(dbName);
    assert.commandWorked(primaryShard.getDB(dbName).runCommand(
        {checkShardingIndex: bucketNss, keyPattern: {[metaField]: 1, [controlTimeField]: 1}}));
    assert.commandFailedWithCode(
        primaryShard.getDB(dbName).runCommand(
            {checkShardingIndex: viewNss, keyPattern: {[metaField]: 1, [controlTimeField]: 1}}),
        ErrorCodes.CommandNotSupportedOnView);
    assert.commandFailed(primaryShard.getDB(dbName).runCommand(
        {checkShardingIndex: bucketNss, keyPattern: {[controlTimeField]: 1}}));
    dropTimeSeriesColl();
})();

// Check we can split/move/merge chunks between shards through bucket namespace but not view
// namespace.
(function checkSplitMoveMergeChunksCommand() {
    createTimeSeriesColl(
        {index: {[metaField]: 1, [timeField]: 1}, shardKey: {[metaField]: 1, [timeField]: 1}});
    const primaryShard = mongo.getPrimaryShard(dbName);
    const otherShard = mongo.getOther(primaryShard);
    const minChunk = {[metaField]: MinKey, [controlTimeField]: MinKey};
    const splitChunk = {[metaField]: numDocsInserted / 2, [controlTimeField]: MinKey};
    const maxChunk = {[metaField]: MaxKey, [controlTimeField]: MaxKey};
    function checkChunkCount(expectedCounts) {
        const counts = mongo.chunkCounts(`system.buckets.${collName}`, dbName);
        assert.docEq(expectedCounts, counts);
    }
    // Command is not supported on the time-series view namespace.
    assert.commandFailedWithCode(mongo.s.adminCommand({split: viewNss, middle: splitChunk}),
                                 ErrorCodes.NamespaceNotSharded);
    assert.commandFailedWithCode(
        mongo.s.adminCommand(
            {moveChunk: viewNss, find: splitChunk, to: otherShard.name, _waitForDelete: true}),
        ErrorCodes.NamespaceNotSharded);
    assert.commandFailedWithCode(
        mongo.s.adminCommand({mergeChunks: viewNss, bounds: [minChunk, maxChunk]}),
        ErrorCodes.NamespaceNotSharded);

    assert.commandWorked(mongo.s.adminCommand({split: bucketNss, middle: splitChunk}));
    checkChunkCount({[primaryShard.shardName]: 2, [otherShard.shardName]: 0});
    assert.commandWorked(mongo.s.adminCommand(
        {moveChunk: bucketNss, find: splitChunk, to: otherShard.name, _waitForDelete: true}));
    checkChunkCount({[primaryShard.shardName]: 1, [otherShard.shardName]: 1});
    assert.commandWorked(mongo.s.adminCommand(
        {moveChunk: bucketNss, find: minChunk, to: otherShard.name, _waitForDelete: true}));
    checkChunkCount({[primaryShard.shardName]: 0, [otherShard.shardName]: 2});
    assert.commandWorked(
        mongo.s.adminCommand({mergeChunks: bucketNss, bounds: [minChunk, maxChunk]}));
    checkChunkCount({[primaryShard.shardName]: 0, [otherShard.shardName]: 1});
    dropTimeSeriesColl();
})();

// Can add control.min.time as the last shard key component on the timeseries collection.
(function checkRefineCollectionShardKeyCommand() {
    createTimeSeriesColl({index: {[metaField]: 1, [timeField]: 1}, shardKey: {[metaField]: 1}});
    assert.commandWorkedOrFailedWithCode(
        mongo.s0.adminCommand(
            {refineCollectionShardKey: viewNss, key: {[metaField]: 1, [controlTimeField]: 1}}),
        ErrorCodes.NamespaceNotSharded /* TODO SERVER-67929 Remove this error code */);
    assert.commandWorked(mongo.s0.adminCommand(
        {refineCollectionShardKey: bucketNss, key: {[metaField]: 1, [controlTimeField]: 1}}));
    const coll = mongo.s0.getDB(dbName)[collName];
    for (let i = 0; i < numDocsInserted; i++) {
        assert.commandWorked(coll.insert({[metaField]: i, [timeField]: ISODate()}));
    }
    assert.eq(numDocsInserted * 2, coll.find({}).count());
    dropTimeSeriesColl();
})();

// Check clearJumboFlag command can clear bucket chunk jumbo flag on bucket namespace but not view
// namespace.
(function checkClearJumboFlagCommand() {
    createTimeSeriesColl(
        {index: {[metaField]: 1, [timeField]: 1}, shardKey: {[metaField]: 1, [timeField]: 1}});
    const configDB = mongo.s0.getDB('config');
    const collDoc = configDB.collections.findOne({_id: bucketNss});
    let chunkDoc = configDB.chunks.findOne({uuid: collDoc.uuid});
    assert.retryNoExcept(() => {
        assert.commandWorked(configDB.chunks.update({_id: chunkDoc._id}, {$set: {jumbo: true}}));
        return true;
    }, "Setting jumbo flag update failed on config server", 10);
    chunkDoc = configDB.chunks.findOne({_id: chunkDoc._id});
    assert(chunkDoc.jumbo);
    assert.commandWorked(
        mongo.s.adminCommand({clearJumboFlag: bucketNss, bounds: [chunkDoc.min, chunkDoc.max]}));
    chunkDoc = configDB.chunks.findOne({_id: chunkDoc._id});
    assert(!chunkDoc.jumbo);
    // Command is not supported on the time-series view namespace.
    assert.commandFailedWithCode(
        mongo.s.adminCommand({clearJumboFlag: viewNss, bounds: [chunkDoc.min, chunkDoc.max]}),
        ErrorCodes.NamespaceNotSharded);
    dropTimeSeriesColl();
})();

// Check renameCollection command cannot modify name through the view namespace.
(function checkRenameCollectionCommand() {
    createTimeSeriesColl(
        {index: {[metaField]: 1, [timeField]: 1}, shardKey: {[metaField]: 1, [timeField]: 1}});
    const newCollName = `${collName}New`;
    const newViewNss = `${dbName}.${newCollName}`;
    // Rename collection is not supported through view namespace.
    assert.commandFailedWithCode(
        mongo.s.adminCommand({renameCollection: viewNss, to: newViewNss}), [
            ErrorCodes.IllegalOperation,
            ErrorCodes.NamespaceNotFound /* TODO SERVER-67929 Remove this error code */
        ]);
    dropTimeSeriesColl();
})();

mongo.stop();
})();
