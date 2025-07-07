/**
 * Test moveRange command on tiemseries collections.
 *
 * @tags: [
 *   requires_timeseries,
 *   # we need 2 shards to perform moveRange
 *   requires_2_or_more_shards,
 *   assumes_no_implicit_collection_creation_on_get_collection,
 *   # TODO SERVER-107141 re-enable this test in stepdown suites
 *   does_not_support_stepdowns,
 * ]
 */

import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {
    getRandomShardName,
    getShardNames,
    setupDbName
} from 'jstests/libs/sharded_cluster_fixture_helpers.js';
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

const coll = db[jsTestName()];
const timeField = 't';

function numChunksOnShard(shardId) {
    const configDB = db.getSiblingDB('config');
    return findChunksUtil.countChunksForNs(
        configDB, getTimeseriesCollForDDLOps(db, coll).getFullName(), {shard: shardId});
}

coll.drop();

assert.commandWorked(db.adminCommand({
    shardCollection: coll.getFullName(),
    key: {[timeField]: 1},
    timeseries: {timeField: timeField}
}));

const primaryShardId = coll.getDB().getDatabasePrimaryShardId();
const otherShardId = getRandomShardName(db, [primaryShardId]);

// Only one chunk on primaryShard
assert.eq(1, numChunksOnShard(primaryShardId));
assert.eq(0, numChunksOnShard(otherShardId));
assert.eq(0, coll.countDocuments({}));

// Move chunk with docs >= year 2000 to otherShard
assert.commandWorked(db.adminCommand({
    moveRange: getTimeseriesCollForDDLOps(db, coll).getFullName(),
    min: {[`control.min.${timeField}`]: ISODate('2000-01-01')},
    max: {[`control.min.${timeField}`]: MaxKey},
    toShard: otherShardId
}));
assert.eq(1, numChunksOnShard(primaryShardId));
assert.eq(1, numChunksOnShard(otherShardId));
assert.eq(0, coll.countDocuments({}));

// Insert 3 docs on otherShard
const docsAfter2000 = [
    {[timeField]: new ISODate('2001-01-01')},
    {[timeField]: new ISODate('2002-01-01')},
    {[timeField]: new ISODate('2003-01-01')},
];
assert.commandWorked(coll.insertMany(docsAfter2000));
assert.sameMembers(docsAfter2000, coll.find({}, {_id: 0}).toArray());

// Insert 3 docs on primaryShard
const docsBefore2000 = [
    {[timeField]: new ISODate('1001-01-01')},
    {[timeField]: new ISODate('1002-01-01')},
    {[timeField]: new ISODate('1003-01-01')},
];
assert.commandWorked(coll.insertMany(docsBefore2000));
assert.sameMembers([...docsBefore2000, ...docsAfter2000], coll.find({}, {_id: 0}).toArray());

// Move chunk with docs >= year 2000 back to primaryShard
assert.commandWorked(db.adminCommand({
    moveRange: getTimeseriesCollForDDLOps(db, coll).getFullName(),
    min: {[`control.min.${timeField}`]: ISODate('2000-01-01')},
    max: {[`control.min.${timeField}`]: MaxKey},
    toShard: primaryShardId
}));
assert.eq(2, numChunksOnShard(primaryShardId));
assert.eq(0, numChunksOnShard(otherShardId));
assert.sameMembers([...docsBefore2000, ...docsAfter2000], coll.find({}, {_id: 0}).toArray());
