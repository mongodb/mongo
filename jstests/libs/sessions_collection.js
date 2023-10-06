// Helpers for testing the logical sessions collection.

/**
 * Assumes that there is only one session chunk and two shards (shard0 and shard1).
 * Returns two shards, where the first shard returned is the shard that has the session chunk
 * and the second shard returned is the one that does not have the session chunk.
 */
export function getShardsWithAndWithoutChunk(st, shard0, shard1) {
    let configDB = st.s.getDB('config');
    let configSessionsUUID = configDB.collections.findOne({_id: 'config.system.sessions'}).uuid;
    let sessionChunks = configDB.chunks.find({uuid: configSessionsUUID}).toArray();

    assert.eq(1, sessionChunks.length, tojson(sessionChunks));
    let sessionChunk = sessionChunks[0];

    if (sessionChunk.shard == shard0.shardName) {
        return {shardWithSessionChunk: shard0, shardWithoutSessionChunk: shard1};
    } else {
        assert.eq(sessionChunk.shard, shard1.shardName, tojson(sessionChunk));
        return {shardWithSessionChunk: shard1, shardWithoutSessionChunk: shard0};
    }
}

/**
 * Validates that the sessions collection exists if we expect it to,
 * and has a TTL index on the lastUse field, if we expect it to.
 */
export function validateSessionsCollection(conn, collectionExists, indexExists, timeout) {
    var config = conn.getDB("config");

    var info = config.getCollectionInfos({name: "system.sessions"});
    var size = collectionExists ? 1 : 0;
    assert.eq(info.length, size);

    var indexes = config.system.sessions.getIndexes();
    var found = false;
    for (var i = 0; i < indexes.length; i++) {
        var entry = indexes[i];
        if (entry["name"] == "lsidTTLIndex") {
            found = true;

            assert.eq(entry["key"], {"lastUse": 1});
            assert(entry.hasOwnProperty("expireAfterSeconds"));
            if (timeout) {
                assert.eq(entry["expireAfterSeconds"], timeout * 60);
            }
        }
    }

    if (indexExists) {
        assert(collectionExists);
        assert(found, "expected sessions collection TTL index to exist");
    } else {
        assert(!found, "TTL index on sessions collection exists");
    }
}
