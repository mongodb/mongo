"use strict";

var CreateShardedCollectionUtil = (function() {
    /**
     * Shards a non-existing collection using the specified shard key and chunk ranges.
     *
     * @param collection - a DBCollection object with an underlying Mongo connection to a mongos.
     * @param chunks - an array of
     * {min: <shardKeyValue0>, max: <shardKeyValue1>, shard: <shardName>} objects. The chunks must
     * form a partition of the {shardKey: MinKey} --> {shardKey: MaxKey} space.
     */
    function shardCollectionWithChunks(collection, shardKey, chunks) {
        assert(collection.getDB().getMongo().isMongos(),
               "collection must have an underlying connection to a mongos");

        const adminDB = collection.getDB().getSiblingDB("admin");
        const configDB = collection.getDB().getSiblingDB("config");

        assert.eq(null,
                  configDB.collections.findOne({ns: collection.getFullName()}),
                  "collection already exists as sharded");
        assert.eq(null, collection.exists(), "collection already exists as unsharded");

        // We include a UUID in the temporary zone names being generated to avoid conflicting with
        // the names of any existing zones.
        const uniquifier = UUID().hex();
        const makeZoneName = (shardName) => `${uniquifier}-${shardName}`;

        const shards = assert.commandWorked(adminDB.runCommand({listShards: 1})).shards;
        for (let shard of shards) {
            assert.commandWorked(
                adminDB.runCommand({addShardToZone: shard._id, zone: makeZoneName(shard._id)}));
        }

        assert.lt(0, chunks.length, "chunks array must not be empty");

        chunks = chunks.slice().sort((a, b) => bsonWoCompare(a.min, b.min));
        assert(Object.values(chunks[0].min).every(x => x === MinKey),
               "first chunk must have all MinKey as min: " + tojson(chunks[0].min));
        assert(Object.values(chunks[chunks.length - 1].max).every(x => x === MaxKey),
               "last chunk must have all MaxKey as max: " + tojson(chunks[chunks.length - 1].max));

        let prevChunk;
        for (let chunk of chunks) {
            if (prevChunk !== undefined) {
                assert.eq(prevChunk.max,
                          chunk.min,
                          "found gap between chunk's max and another chunk's min");
            }

            assert.commandWorked(adminDB.runCommand({
                updateZoneKeyRange: collection.getFullName(),
                min: chunk.min,
                max: chunk.max,
                zone: makeZoneName(chunk.shard),
            }));
            prevChunk = chunk;
        }

        assert.commandWorked(adminDB.runCommand({enableSharding: collection.getDB().getName()}));
        assert.commandWorked(adminDB.runCommand({
            shardCollection: collection.getFullName(),
            key: shardKey,
            presplitHashedZones: false,
        }));

        // We disassociate the chunk ranges from the zones to allow removing the zones altogether.
        for (let chunk of chunks) {
            assert.commandWorked(adminDB.runCommand({
                updateZoneKeyRange: collection.getFullName(),
                min: chunk.min,
                max: chunk.max,
                zone: null,
            }));
        }

        for (let shard of shards) {
            assert.commandWorked(adminDB.runCommand(
                {removeShardFromZone: shard._id, zone: makeZoneName(shard._id)}));
        }
    }

    return {
        shardCollectionWithChunks,
    };
})();
