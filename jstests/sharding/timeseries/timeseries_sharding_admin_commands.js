/**
 * Test sharding admin commands on sharded time-series collection.
 *
 * @tags: [
 *   requires_fcv_51
 * ]
 */

import {
    areViewlessTimeseriesEnabled,
    getTimeseriesCollForDDLOps
} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Connections.
const mongo = new ShardingTest({shards: 2, rs: {nodes: 3}});
const dbName = 'testDB';
const collName = 'testColl';
const timeField = 'time';
const metaField = 'meta';
const collNss = `${dbName}.${collName}`;
const controlTimeField = `control.min.${timeField}`;
const numDocsInserted = 20;
const zone = 'Z';
assert.commandWorked(mongo.s0.adminCommand({enableSharding: dbName}));
assert.commandWorked(mongo.s0.adminCommand({addShardToZone: mongo.shard0.shardName, zone: zone}));
const db = mongo.s0.getDB(dbName);
const coll = db.getCollection(collName);

function createTimeSeriesColl({index, shardKey}) {
    assert.commandWorked(
        db.createCollection(collName, {timeseries: {timeField: timeField, metaField: metaField}}));
    assert.commandWorked(db[collName].createIndex(index));
    for (let i = 0; i < numDocsInserted; i++) {
        assert.commandWorked(db[collName].insert({[metaField]: i, [timeField]: ISODate()}));
    }
    if (shardKey) {
        assert.commandWorked(mongo.s0.adminCommand({
            shardCollection: collNss,
            key: shardKey,
            timeseries: {timeField: timeField, metaField: metaField}
        }));
    }
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

const zoneShardingTestCases = [
    // For time-series collections sharded by metaField + timeField, updating a zone range on the
    // time field other than MinKey -> MinKey prevents sharding. This is necessary to ensure that we
    // never write measurements into a shard outside of a zone range.
    {
        shardKey: {[metaField]: 1, [timeField]: 1},
        index: {[metaField]: 1, [timeField]: 1},
        min: {[metaField]: 1, [controlTimeField]: 1},
        max: {[metaField]: 10, [controlTimeField]: 10},
        worksWhenUpdatingZoneKeyRangeBeforeSharding: false,
        worksWhenUpdatingZoneKeyRangeAfterSharding: false,
    },
    {
        shardKey: {[metaField]: 1, [timeField]: 1},
        index: {[metaField]: 1, [timeField]: 1},
        min: {[metaField]: 1, [controlTimeField]: 1},
        max: {[metaField]: 10, [controlTimeField]: MaxKey},
        worksWhenUpdatingZoneKeyRangeBeforeSharding: false,
        worksWhenUpdatingZoneKeyRangeAfterSharding: false,
    },
    {
        shardKey: {[metaField]: 1, [timeField]: 1},
        index: {[metaField]: 1, [timeField]: 1},
        min: {[metaField]: 1, [controlTimeField]: MinKey},
        max: {[metaField]: 10, [controlTimeField]: 10},
        worksWhenUpdatingZoneKeyRangeBeforeSharding: false,
        worksWhenUpdatingZoneKeyRangeAfterSharding: false,
    },
    {
        shardKey: {[metaField]: 1, [timeField]: 1},
        index: {[metaField]: 1, [timeField]: 1},
        min: {[metaField]: 1, [controlTimeField]: MinKey},
        max: {[metaField]: 10, [controlTimeField]: MaxKey},
        worksWhenUpdatingZoneKeyRangeBeforeSharding: false,
        worksWhenUpdatingZoneKeyRangeAfterSharding: false,
    },
    {
        shardKey: {[metaField]: 1, [timeField]: 1},
        index: {[metaField]: 1, [timeField]: 1},
        min: {[metaField]: 1, [controlTimeField]: MinKey},
        max: {[metaField]: 10, [controlTimeField]: MinKey},
        worksWhenUpdatingZoneKeyRangeBeforeSharding: true,
        worksWhenUpdatingZoneKeyRangeAfterSharding: true,
    },
    {
        shardKey: {[timeField]: 1},
        index: {[timeField]: 1},
        min: {[controlTimeField]: 1},
        max: {[controlTimeField]: 10},
        worksWhenUpdatingZoneKeyRangeBeforeSharding: false,
        worksWhenUpdatingZoneKeyRangeAfterSharding: false,
    },
    // Updating a zone with a metaField range works for time-series collections sharded on metaField
    {
        shardKey: {[metaField]: 1, [timeField]: 1},
        index: {[metaField]: 1, [timeField]: 1},
        min: {[metaField]: 1},
        max: {[metaField]: 10},
        // Sharding a collection fails if the predefined zones don't exactly match the shard key,
        // but not vice versa. Note this behavior applies to all collections, not just time-series.
        worksWhenUpdatingZoneKeyRangeBeforeSharding: false,
        worksWhenUpdatingZoneKeyRangeAfterSharding: true,
    },
    {
        shardKey: {[metaField]: 1},
        index: {[metaField]: 1},
        min: {[metaField]: 1},
        max: {[metaField]: 10},
        worksWhenUpdatingZoneKeyRangeBeforeSharding: true,
        worksWhenUpdatingZoneKeyRangeAfterSharding: true,
    },
    {
        shardKey: {[metaField + ".xyz"]: 1},
        index: {[metaField + ".xyz"]: 1},
        min: {[metaField + ".xyz"]: 1},
        max: {[metaField + ".xyz"]: 10},
        worksWhenUpdatingZoneKeyRangeBeforeSharding: true,
        worksWhenUpdatingZoneKeyRangeAfterSharding: true,
    },
];

// Check updateZoneKeyRange with range other than MinKey -> MinKey on the time field prevents
// sharding. The last successful call will shard the collection.
(function checkUpdateZoneKeyRangeCommandBeforeSharding() {
    for (const testCase of zoneShardingTestCases) {
        jsTest.log(`Running test case, updating zones before sharding: ${tojsononeline(testCase)}`);
        const {shardKey, index, min, max, worksWhenUpdatingZoneKeyRangeBeforeSharding} = testCase;
        createTimeSeriesColl({index: index});
        assert.commandWorked(mongo.s0.adminCommand({
            updateZoneKeyRange: getTimeseriesCollForDDLOps(db, coll).getFullName(),
            min: min,
            max: max,
            zone: zone
        }));
        const tag = mongo.s0.getDB('config').tags.findOne(
            {ns: getTimeseriesCollForDDLOps(db, coll).getFullName()});
        assertRangeMatch(tag.min, min);
        assertRangeMatch(tag.max, max);
        const result = mongo.s0.adminCommand({
            shardCollection: collNss,
            key: shardKey,
            timeseries: {timeField: timeField, metaField: metaField}
        });
        if (worksWhenUpdatingZoneKeyRangeBeforeSharding) {
            assert.commandWorked(result);
        } else {
            assert.commandFailedWithCode(result, ErrorCodes.InvalidOptions);
        }
        assert.commandWorked(mongo.s0.adminCommand({
            updateZoneKeyRange: getTimeseriesCollForDDLOps(db, coll).getFullName(),
            min: min,
            max: max,
            zone: null
        }));
        assert.eq(0,
                  mongo.s0.getDB('config')
                      .tags.find({ns: getTimeseriesCollForDDLOps(db, coll).getFullName()})
                      .count());
        assert(coll.drop());
    }
})();

// Check updateZoneKeyRange rejects range other than MinKey -> MinKey on the time field after
// sharding.
(function checkUpdateZoneKeyRangeAfterSharding() {
    for (const testCase of zoneShardingTestCases) {
        jsTest.log(`Running test case, updating zones after sharding: ${tojsononeline(testCase)}`);
        const {shardKey, index, min, max, worksWhenUpdatingZoneKeyRangeAfterSharding} = testCase;
        createTimeSeriesColl({index: index, shardKey: shardKey});
        const result = mongo.s0.adminCommand({
            updateZoneKeyRange: getTimeseriesCollForDDLOps(db, coll).getFullName(),
            min: min,
            max: max,
            zone: zone
        });
        if (worksWhenUpdatingZoneKeyRangeAfterSharding) {
            assert.commandWorked(result);
            const tag = mongo.s0.getDB('config').tags.findOne(
                {ns: getTimeseriesCollForDDLOps(db, coll).getFullName()});
            assertRangeMatch(tag.min, min);
            assertRangeMatch(tag.max, max);
            assert.commandWorked(mongo.s0.adminCommand({
                updateZoneKeyRange: getTimeseriesCollForDDLOps(db, coll).getFullName(),
                min: min,
                max: max,
                zone: null
            }));
            assert.eq(0,
                      mongo.s0.getDB('config')
                          .tags.find({ns: getTimeseriesCollForDDLOps(db, coll).getFullName()})
                          .count());
        } else {
            assert.commandFailedWithCode(result, ErrorCodes.InvalidOptions);
        }
        assert(coll.drop());
    }
})();

// Check shardingState commands returns the expected collection info.
(function checkShardingStateCommand() {
    createTimeSeriesColl(
        {index: {[metaField]: 1, [timeField]: 1}, shardKey: {[metaField]: 1, [timeField]: 1}});
    assert.commandWorked(
        mongo.getPrimaryShard(dbName).adminCommand({_flushRoutingTableCacheUpdates: collNss}));
    if (!areViewlessTimeseriesEnabled(mongo.s.getDB(dbName))) {
        assert.commandWorked(mongo.getPrimaryShard(dbName).adminCommand(
            {_flushRoutingTableCacheUpdates: getTimeseriesCollForDDLOps(db, coll).getFullName()}));
    }
    const shardingStateRes = mongo.getPrimaryShard(dbName).adminCommand({shardingState: 1});
    const shardingStateColls = shardingStateRes.versions;

    const isNssSharded = nss =>
        (nss in shardingStateColls &&
         timestampCmp(shardingStateColls[nss]["placementVersion"], Timestamp(0, 0)) !== 0);
    assert(isNssSharded(getTimeseriesCollForDDLOps(db, coll).getFullName()));

    // TODO SERVER-101784 Remove this check once only viewless timeseries exist.
    // Note: for legacy timeseries this checks that the view namespace is not sharded.
    if (!areViewlessTimeseriesEnabled(mongo.s.getDB(dbName))) {
        assert(!isNssSharded(collNss));
    }
    assert(coll.drop());
})();

// Check reshardCollection commands are disabled for time-series collection.
(function checkReshardCollectionCommand() {
    if (!FeatureFlagUtil.isPresentAndEnabled(mongo.s.getDB('admin'), 'ReshardingForTimeseries')) {
        createTimeSeriesColl({index: {[metaField]: 1, [timeField]: 1}, shardKey: {[metaField]: 1}});
        assert.commandFailedWithCode(
            mongo.s0.adminCommand(
                {reshardCollection: collNss, key: {[metaField]: 1, [controlTimeField]: 1}}),
            [ErrorCodes.NotImplemented, ErrorCodes.IllegalOperation]);
        // TODO SERVER-107138 Ensure that resharding fails when issued on the buckets
        // collection on FCV 9.0.
        if (!areViewlessTimeseriesEnabled(mongo.s.getDB(dbName))) {
            assert.commandFailedWithCode(mongo.s0.adminCommand({
                reshardCollection: getTimeseriesCollForDDLOps(db, coll).getFullName(),
                key: {[metaField]: 1, [controlTimeField]: 1}
            }),
                                         [ErrorCodes.NotImplemented, ErrorCodes.IllegalOperation]);
        }
        assert(coll.drop());
    } else {
        jsTestLog(`Skipping resharding for timeseries not implemented test.`);
    }
})();

// Check checkShardingIndex works for the correct key pattern and fails for an incorrect one.
(function checkCheckShardingIndexCommand() {
    createTimeSeriesColl(
        {index: {[metaField]: 1, [timeField]: 1}, shardKey: {[metaField]: 1, [timeField]: 1}});
    const primaryShard = mongo.getPrimaryShard(dbName);
    assert.commandWorked(primaryShard.getDB(dbName).runCommand({
        checkShardingIndex: getTimeseriesCollForDDLOps(db, coll).getFullName(),
        keyPattern: {[metaField]: 1, [controlTimeField]: 1}
    }));
    assert.commandFailed(primaryShard.getDB(dbName).runCommand({
        checkShardingIndex: getTimeseriesCollForDDLOps(db, coll).getFullName(),
        keyPattern: {[controlTimeField]: 1}
    }));
    if (!areViewlessTimeseriesEnabled(mongo.s.getDB(dbName))) {
        assert.commandFailedWithCode(
            primaryShard.getDB(dbName).runCommand(
                {checkShardingIndex: collNss, keyPattern: {[metaField]: 1, [controlTimeField]: 1}}),
            ErrorCodes.CommandNotSupportedOnView);
    }
    assert(coll.drop());
})();

// Check we can split/move/merge chunks between shards.
(function checkSplitMoveMergeChunksCommand() {
    createTimeSeriesColl(
        {index: {[metaField]: 1, [timeField]: 1}, shardKey: {[metaField]: 1, [timeField]: 1}});
    const primaryShard = mongo.getPrimaryShard(dbName);
    const otherShard = mongo.getOther(primaryShard);
    const minChunk = {[metaField]: MinKey, [controlTimeField]: MinKey};
    const splitChunk = {[metaField]: numDocsInserted / 2, [controlTimeField]: MinKey};
    const maxChunk = {[metaField]: MaxKey, [controlTimeField]: MaxKey};
    function checkChunkCount(expectedCounts) {
        const counts = mongo.chunkCounts(collName, dbName);
        assert.docEq(expectedCounts, counts);
    }
    // TODO SERVER-106896 Consider re-enabling the assertions below.
    if (!areViewlessTimeseriesEnabled(mongo.s.getDB(dbName))) {
        // Command is not supported on the time-series view namespace.
        assert.commandFailedWithCode(mongo.s.adminCommand({split: collNss, middle: splitChunk}),
                                     ErrorCodes.NamespaceNotSharded);
        assert.commandFailedWithCode(
            mongo.s.adminCommand(
                {moveChunk: collNss, find: splitChunk, to: otherShard.name, _waitForDelete: true}),
            ErrorCodes.NamespaceNotSharded);
        assert.commandFailedWithCode(
            mongo.s.adminCommand({mergeChunks: collNss, bounds: [minChunk, maxChunk]}),
            ErrorCodes.NamespaceNotSharded);
    }

    assert.commandWorked(mongo.s.adminCommand(
        {split: getTimeseriesCollForDDLOps(db, coll).getFullName(), middle: splitChunk}));
    checkChunkCount({[primaryShard.shardName]: 2, [otherShard.shardName]: 0});
    assert.commandWorked(mongo.s.adminCommand({
        moveChunk: getTimeseriesCollForDDLOps(db, coll).getFullName(),
        find: splitChunk,
        to: otherShard.name,
        _waitForDelete: true
    }));
    checkChunkCount({[primaryShard.shardName]: 1, [otherShard.shardName]: 1});
    assert.commandWorked(mongo.s.adminCommand({
        moveChunk: getTimeseriesCollForDDLOps(db, coll).getFullName(),
        find: minChunk,
        to: otherShard.name,
        _waitForDelete: true
    }));
    checkChunkCount({[primaryShard.shardName]: 0, [otherShard.shardName]: 2});
    assert.commandWorked(mongo.s.adminCommand({
        mergeChunks: getTimeseriesCollForDDLOps(db, coll).getFullName(),
        bounds: [minChunk, maxChunk]
    }));
    checkChunkCount({[primaryShard.shardName]: 0, [otherShard.shardName]: 1});
    assert(coll.drop());
})();

// Can add control.min.time as the last shard key component on the timeseries collection.
(function checkRefineCollectionShardKeyCommand() {
    createTimeSeriesColl({index: {[metaField]: 1, [timeField]: 1}, shardKey: {[metaField]: 1}});
    assert.commandWorked(mongo.s0.adminCommand(
        {refineCollectionShardKey: collNss, key: {[metaField]: 1, [controlTimeField]: 1}}));
    if (!areViewlessTimeseriesEnabled(mongo.s.getDB(dbName))) {
        assert.commandWorked(mongo.s0.adminCommand({
            refineCollectionShardKey: getTimeseriesCollForDDLOps(db, coll).getFullName(),
            key: {[metaField]: 1, [controlTimeField]: 1}
        }));
    }
    for (let i = 0; i < numDocsInserted; i++) {
        assert.commandWorked(coll.insert({[metaField]: i, [timeField]: ISODate()}));
    }
    assert.eq(numDocsInserted * 2, coll.find({}).count());
    assert(coll.drop());
})();

// Check clearJumboFlag command can clear chunk jumbo flag.
(function checkClearJumboFlagCommand() {
    createTimeSeriesColl(
        {index: {[metaField]: 1, [timeField]: 1}, shardKey: {[metaField]: 1, [timeField]: 1}});
    const configDB = mongo.s0.getDB('config');
    const collDoc =
        configDB.collections.findOne({_id: getTimeseriesCollForDDLOps(db, coll).getFullName()});
    let chunkDoc = configDB.chunks.findOne({uuid: collDoc.uuid});
    assert.retryNoExcept(() => {
        assert.commandWorked(configDB.chunks.update({_id: chunkDoc._id}, {$set: {jumbo: true}}));
        return true;
    }, "Setting jumbo flag update failed on config server", 10);
    chunkDoc = configDB.chunks.findOne({_id: chunkDoc._id});
    assert(chunkDoc.jumbo);
    assert.commandWorked(mongo.s.adminCommand({
        clearJumboFlag: getTimeseriesCollForDDLOps(db, coll).getFullName(),
        bounds: [chunkDoc.min, chunkDoc.max]
    }));
    chunkDoc = configDB.chunks.findOne({_id: chunkDoc._id});
    assert(!chunkDoc.jumbo);
    // TODO SERVER-106896 Consider re-enabling the assertion below.
    if (!areViewlessTimeseriesEnabled(mongo.s.getDB(dbName))) {
        // Command is not supported on the time-series view namespace.
        assert.commandFailedWithCode(
            mongo.s.adminCommand({clearJumboFlag: collNss, bounds: [chunkDoc.min, chunkDoc.max]}),
            ErrorCodes.NamespaceNotSharded);
    }
    assert(coll.drop());
})();

mongo.stop();
