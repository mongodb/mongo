// Helpers for testing the logical sessions collection.

/**
 * Validates that the sessions collection exists if we expect it to,
 * and has a TTL index on the lastUse field, if we expect it to.
 */
function validateSessionsCollection(conn, collectionExists, indexExists) {
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

            assert.eq(entry["ns"], "config.system.sessions");
            assert.eq(entry["key"], {"lastUse": 1});
            assert(entry.hasOwnProperty("expireAfterSeconds"));
        }
    }

    if (indexExists) {
        assert(collectionExists);
        assert(found, "expected sessions collection TTL index to exist");
    } else {
        assert(!found, "TTL index on sessions collection exists");
    }
}
