'use strict';

/**
 * Runs refineCollectionShardKey and zone operations concurrently.
 *
 * States:
 *  - sendZoneToOtherShard: Picks a random zone assigned to this thread, removes it from the
 *    current shard, and assigns it to the other shard. Verifies via querying the config server
 *    that the first shard no longer has said zone, and that the second shard now has said zone.
 *
 *  - swapZoneRange: Removes the ranges from each of the zones assigned to this thread, and swaps
 *    them, such that each range is now assigned to the opposite zone. Verifies that the zones
 *    have swapped ranges by querying the config server.
 *
 *  - refineCollectionShardKey - Refines the collection's shard key and decreases the latch count
 *    such that the next latch collection will be targeted by the test.
 *
 * @tags: [requires_persistence, requires_sharding, requires_fcv_44]
 */

load('jstests/libs/parallelTester.js');

var $config = (function() {
    var data = {
        oldShardKeyField: 'a',
        newShardKeyFields: ['a', 'b'],
        oldShardKey: {a: 1},
        newShardKey: {a: 1, b: 1},
        docCount: 100,
        shardNames: [],
        zonesMappedToShardsForCollection: {},
        zonesMappedToRangesOwnedByThreadForCollection: {},
    };

    function getCurrentLatchCollName(collName, latch) {
        return collName + '_' + latch.getCount().toString();
    }

    function getCurrentOrPreviousLatchCollName(collName, latch, latchCount) {
        const latchNumber =
            (Math.random() < 0.5) ? latch.getCount() : Math.min(latch.getCount() + 1, latchCount);

        return collName + '_' + latchNumber.toString();
    }

    function getConfigTagsCollection(db) {
        return db.getSiblingDB('config').tags;
    }

    function getConfigShardsCollection(db) {
        return db.getSiblingDB('config').shards;
    }

    function populateTagRangesForThreadFromConfig(
        db, collName, threadId, currentZoneRangeMapForCollection) {
        const threadZoneRegexMatch = '.*tid-' + threadId + '.*';
        const tags = getConfigTagsCollection(db)
                         .find({ns: db + '.' + collName, tag: {$regex: threadZoneRegexMatch}})
                         .toArray();
        assertAlways.eq(2, tags.length);
        tags.forEach((tag) => {
            currentZoneRangeMapForCollection[tag.tag] = {'min': tag.min, 'max': tag.max};
        });
    }

    function initMapsForThread(db,
                               collName,
                               threadId,
                               shardNames,
                               zonesMappedToShardsForCollection,
                               zonesMappedToRangesOwnedByThreadForCollection) {
        const threadZoneStringMatch = 'tid-' + threadId;

        if (!zonesMappedToShardsForCollection[collName]) {
            zonesMappedToShardsForCollection[collName] = {};
        }
        let currentZoneShardMap = zonesMappedToShardsForCollection[collName];

        const shards = getConfigShardsCollection(db).find({});
        shards.forEach((shard) => {
            if (!shardNames.includes(shard._id)) {
                shardNames.push(shard._id);
            }

            shard.tags.forEach((tag) => {
                if (!tag.includes(threadZoneStringMatch)) {
                    return;
                }
                currentZoneShardMap[tag] = shard._id;
            });
        });

        if (!zonesMappedToRangesOwnedByThreadForCollection[collName]) {
            zonesMappedToRangesOwnedByThreadForCollection[collName] = {};
        }

        let currentZoneRangeMap = zonesMappedToRangesOwnedByThreadForCollection[collName];
        populateTagRangesForThreadFromConfig(db, collName, threadId, currentZoneRangeMap);
    }

    function attemptSwapZoneRange(db, collName, zonesMappedToRangesOwnedByThreadForCollection) {
        const fullCollName = db + '.' + collName;

        // Assume that we only have two zones owned by the thread for a given collection.
        const zoneKeys = Object.keys(zonesMappedToRangesOwnedByThreadForCollection);
        assertAlways.eq(2, zoneKeys.length);

        const firstZoneName = zoneKeys[0];
        const firstZoneRange = zonesMappedToRangesOwnedByThreadForCollection[zoneKeys[0]];
        const secondZoneName = zoneKeys[1];
        const secondZoneRange = zonesMappedToRangesOwnedByThreadForCollection[zoneKeys[1]];

        // Swap the zone ranges by first setting both to null, then reversing the values such that
        // the first zone's range will now be associated with the second zone, and vice versa.
        assertAlways.commandWorked(db.adminCommand({
            updateZoneKeyRange: fullCollName,
            min: firstZoneRange.min,
            max: firstZoneRange.max,
            zone: null
        }));

        assertAlways.commandWorked(db.adminCommand({
            updateZoneKeyRange: fullCollName,
            min: secondZoneRange.min,
            max: secondZoneRange.max,
            zone: null
        }));

        assertAlways.commandWorked(db.adminCommand({
            updateZoneKeyRange: fullCollName,
            min: secondZoneRange.min,
            max: secondZoneRange.max,
            zone: firstZoneName
        }));
        assertAlways.commandWorked(db.adminCommand({
            updateZoneKeyRange: fullCollName,
            min: firstZoneRange.min,
            max: firstZoneRange.max,
            zone: secondZoneName
        }));

        // Verify that the commands set the correct data on the config server.
        const firstTagRangeOnConfig = getConfigTagsCollection(db).findOne({tag: firstZoneName});
        const secondTagRangeOnConfig = getConfigTagsCollection(db).findOne({tag: secondZoneName});

        assertAlways.eq(secondZoneRange.min, firstTagRangeOnConfig.min);
        assertAlways.eq(secondZoneRange.max, firstTagRangeOnConfig.max);
        assertAlways.eq(firstZoneRange.min, secondTagRangeOnConfig.min);
        assertAlways.eq(firstZoneRange.max, secondTagRangeOnConfig.max);

        zonesMappedToRangesOwnedByThreadForCollection[firstZoneName] = secondZoneRange;
        zonesMappedToRangesOwnedByThreadForCollection[secondZoneName] = firstZoneRange;
    }

    const states = {
        init: function init(db, collName, connCache) {
            for (let i = this.latchCount; i >= 0; --i) {
                initMapsForThread(db,
                                  collName + '_' + i,
                                  this.tid,
                                  this.shardNames,
                                  this.zonesMappedToShardsForCollection,
                                  this.zonesMappedToRangesOwnedByThreadForCollection);
            }
        },

        sendZoneToOtherShard: function sendZoneToOtherShard(db, collName, connCache) {
            const configShardsCollection = getConfigShardsCollection(db);

            const latchCollName =
                getCurrentOrPreviousLatchCollName(collName, this.latch, this.latchCount);
            let currentZoneShardMap = this.zonesMappedToShardsForCollection[latchCollName];

            const zoneKeys = Object.keys(currentZoneShardMap);
            const randomZone = zoneKeys[Random.randInt(zoneKeys.length)];
            const formerShardForZone = currentZoneShardMap[randomZone];

            // We assume here that we only have two shards.
            let newShardForZone = this.shardNames.filter((shard) => {
                if (shard !== formerShardForZone) {
                    return shard;
                }
            })[0];

            // Move the zone to the other shard.
            assertAlways.commandWorked(
                db.adminCommand({addShardToZone: newShardForZone, zone: randomZone}));
            assertAlways.commandWorked(
                db.adminCommand({removeShardFromZone: formerShardForZone, zone: randomZone}));

            // Verify that the zone exists only on the new shard.
            const tagsForFormerShard =
                configShardsCollection.findOne({_id: formerShardForZone}).tags;
            const tagsForNewShard = configShardsCollection.findOne({_id: newShardForZone}).tags;

            assertAlways.eq(false, tagsForFormerShard.includes(randomZone));
            assertAlways.eq(true, tagsForNewShard.includes(randomZone));

            currentZoneShardMap[randomZone] = newShardForZone;
        },

        swapZoneRange: function swapZoneRange(db, collName, connCache) {
            const latchCollName =
                getCurrentOrPreviousLatchCollName(collName, this.latch, this.latchCount);

            let currentZoneRangeMap =
                this.zonesMappedToRangesOwnedByThreadForCollection[latchCollName];

            try {
                attemptSwapZoneRange(db, latchCollName, currentZoneRangeMap);
            } catch (e) {
                // During the process of attempting to swap the zone range, the collection may
                // become refined. Retrying swapping the zone range will allow us to target the
                // shard key in its refined state.
                const newShardKeyField = this.newShardKeyFields[1];
                if (e.message.includes(newShardKeyField) && e.message.includes('are not equal')) {
                    jsTestLog("Retrying swapZoneRange on collection " + latchCollName +
                              " due to refineCollectionShardKey conflict");
                    for (let zoneRange of Object.values(currentZoneRangeMap)) {
                        zoneRange.min[newShardKeyField] = MinKey;
                        zoneRange.max[newShardKeyField] = MinKey;
                    }
                    // Try swapping the zone range only once more.
                    attemptSwapZoneRange(db, latchCollName, currentZoneRangeMap);
                } else {
                    throw e;
                }
            }
        },

        refineCollectionShardKey: function refineCollectionShardKey(db, collName, connCache) {
            const latchCollName = getCurrentLatchCollName(collName, this.latch);
            const latchColl = db.getCollection(latchCollName);

            try {
                assertAlways.commandWorked(db.adminCommand(
                    {refineCollectionShardKey: latchColl.getFullName(), key: this.newShardKey}));
            } catch (e) {
                // There is a race that could occur where two threads run refineCollectionShardKey
                // concurrently on the same collection. Since the epoch of the collection changes,
                // the later thread may receive a StaleEpoch error, which is an acceptable error.
                if (e.code == ErrorCodes.StaleEpoch) {
                    print("Ignoring acceptable refineCollectionShardKey error: " + tojson(e));
                    return;
                }
                throw e;
            }

            this.latch.countDown();
        }
    };

    const transitions = {
        init: {sendZoneToOtherShard: 0.4, swapZoneRange: 0.4, refineCollectionShardKey: 0.2},
        sendZoneToOtherShard:
            {sendZoneToOtherShard: 0.4, swapZoneRange: 0.4, refineCollectionShardKey: 0.2},
        swapZoneRange:
            {sendZoneToOtherShard: 0.4, swapZoneRange: 0.4, refineCollectionShardKey: 0.2},
        refineCollectionShardKey:
            {sendZoneToOtherShard: 0.4, swapZoneRange: 0.4, refineCollectionShardKey: 0.2},
    };

    function setup(db, collName, cluster) {
        // Use a CountDownLatch as if it were a std::atomic<long long> shared between all of the
        // threads. The collection name is suffixed with the current this.latch.getCount() value
        // when concurrent operations are run against it. With every refineCollectionShardKey, call
        // this.latch.countDown() and run CRUD operations against the new collection suffixed with
        // this.latch.getCount(). This bypasses the need to drop and reshard the current collection
        // with every refineCollectionShardKey since it cannot be achieved in an atomic fashion
        // fashion under the FSM infrastructure (meaning operations could fail).
        this.latchCount = this.iterations;
        this.latch = new CountDownLatch(this.latchCount);
        this.zoneAndRangeCount = this.threadCount * 2;
        this.partitionSize = this.docCount / this.zoneAndRangeCount;

        const shardNames = Object.keys(cluster.getSerializedCluster().shards);

        // Proactively create and shard all possible collections suffixed with this.latch.getCount()
        // that could receive CRUD operations over the course of the FSM workload. This prevents the
        // race that could occur between sharding a collection and creating an index on the new
        // shard key (if this step were done after every refineCollectionShardKey).
        for (let i = this.latchCount; i >= 0; --i) {
            const latchCollName = collName + '_' + i;
            const latchColl = db.getCollection(latchCollName);

            let currentRangeLowerBound = 0;
            for (let j = 0; j < this.zoneAndRangeCount; ++j) {
                // Create a name for the zone that guarantees that the zone will never be touched
                // by other threads for this test.
                const currentThread = Math.floor(j / 2);
                const zoneName = latchCollName + '-tid-' + currentThread + '-' + j % 2;

                // Add the zone to one random shard.
                const randomShard = shardNames[Random.randInt(shardNames.length)];
                assertAlways.commandWorked(
                    db.adminCommand({addShardToZone: randomShard, zone: zoneName}));

                // Assign a range to the zone.
                const lowerZoneRange = {[this.oldShardKeyField]: currentRangeLowerBound};
                const uppperZoneRange =
                    {[this.oldShardKeyField]: currentRangeLowerBound + this.partitionSize};
                assertAlways.commandWorked(db.adminCommand({
                    updateZoneKeyRange: latchColl.getFullName(),
                    min: lowerZoneRange,
                    max: uppperZoneRange,
                    zone: zoneName
                }));

                currentRangeLowerBound += this.partitionSize;
            }

            // Shard the collection, implicitly creating chunks to match the created zones.
            assertAlways.commandWorked(
                db.adminCommand({shardCollection: latchColl.getFullName(), key: this.oldShardKey}));
            assertAlways.commandWorked(latchColl.createIndex(this.newShardKey));

            db.printShardingStatus();
        }
    }

    return {
        threadCount: 5,
        iterations: 25,
        data: data,
        startState: 'init',
        states: states,
        transitions: transitions,
        setup: setup,
        passConnectionCache: true,
    };
})();
