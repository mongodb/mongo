/**
 * Tests that inserts into a sharded timeseries collection with zones go into the defined shards.
 * @tags: [
 *   requires_2_or_more_shards,
 *   assumes_stable_shard_list,
 *   assumes_unsharded_collection,
 *   # Older versions lack proper support for zones (see SERVER-94974)
 *   requires_fcv_82,
 *   # TODO(SERVER-107912): Enable this test on suites with random migrations
 *   assumes_balancer_off,
 * ]
 */

import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {getShardNames} from 'jstests/libs/sharded_cluster_fixture_helpers.js';

describe('Basic timeseries zone sharding test', function() {
    const timeField = 'time';
    const metaField = 'md';

    const zones = ['A', 'B'];
    const zoneCutoff = 123;

    const docs = [
        {[timeField]: ISODate(), [metaField]: zoneCutoff - 100},
        {[timeField]: ISODate(), [metaField]: zoneCutoff + 42},
        {[timeField]: ISODate(), [metaField]: zoneCutoff - 1},
        {[timeField]: ISODate(), [metaField]: zoneCutoff}
    ];

    let shardNames;

    const defineZones = coll => {
        assert.commandWorked(db.adminCommand({
            updateZoneKeyRange: getTimeseriesCollForDDLOps(db, coll).getFullName(),
            min: {"meta": MinKey()},
            max: {"meta": zoneCutoff},
            zone: zones[0],
            // TODO(SERVER-107478): Use rawData mode
        }));
        assert.commandWorked(db.adminCommand({
            updateZoneKeyRange: getTimeseriesCollForDDLOps(db, coll).getFullName(),
            min: {"meta": zoneCutoff},
            max: {"meta": MaxKey()},
            zone: zones[1],
            // TODO(SERVER-107478): Use rawData mode
        }));
    };

    const populateShardedTimeseriesCollection = coll => {
        assert.commandWorked(db.adminCommand({
            shardCollection: coll.getFullName(),
            key: {[metaField]: 1},
            timeseries: {timeField: timeField, metaField: metaField}
        }));

        assert.commandWorked(coll.insertMany(docs));
    };

    const areChunksDistributedByZones = coll => {
        const chunks = db.getSiblingDB("config")
                           .chunks.find({uuid: getTimeseriesCollForDDLOps(db, coll).getUUID()})
                           .toArray();
        jsTestLog("Chunks: " + tojson(chunks));

        const chunksOnShard0 = chunks.filter(c => c.shard === shardNames[0]);
        const chunksOnShard1 = chunks.filter(c => c.shard === shardNames[1]);

        return chunksOnShard0.every(c => bsonWoCompare(c.max.meta, zoneCutoff) <= 0) &&
            chunksOnShard1.every(c => bsonWoCompare(c.min.meta, zoneCutoff) >= 0);
    };

    before(function() {
        shardNames = getShardNames(db);
        assert.commandWorked(db.adminCommand({addShardToZone: shardNames[0], zone: zones[0]}));
        assert.commandWorked(db.adminCommand({addShardToZone: shardNames[1], zone: zones[1]}));
    });

    after(function() {
        db.dropDatabase();  // Drops the zone key ranges, so we can remove the shards from the zones
        assert.commandWorked(db.adminCommand({removeShardFromZone: shardNames[0], zone: zones[0]}));
        assert.commandWorked(db.adminCommand({removeShardFromZone: shardNames[1], zone: zones[1]}));
    });

    it('should distribute the chunks by zones when sharding with predefined zones', function() {
        const coll = db.getCollection('zoneThenShard');
        coll.drop();
        defineZones(coll);
        populateShardedTimeseriesCollection(coll);
        // shardCollection creates chunks according to the zones,
        // so we can immediately assert that the chunks match the expected distribution.
        assert(areChunksDistributedByZones(coll));
        assert.sameMembers(docs, coll.find({}, {_id: 0}).toArray());
    });

    it('should distribute the chunks by zones when defining zones after sharding', function() {
        if (assert.commandWorked(db.adminCommand({balancerStatus: 1})).mode === 'off') {
            jsTestLog("Skipping test case because the balancer is disabled.");
            return;
        }

        const coll = db.getCollection('zoneAfterSharding');
        coll.drop();
        populateShardedTimeseriesCollection(coll);
        defineZones(coll);
        // We must wait for the balancer to split and move the chunks according to the defined zones
        assert.soon(() => areChunksDistributedByZones(coll));
        assert.sameMembers(docs, coll.find({}, {_id: 0}).toArray());
    });
});
