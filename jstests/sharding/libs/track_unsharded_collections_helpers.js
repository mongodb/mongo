/**
 * Common helpers for testing operations on unsharded collections.
 */

function getNewNs(dbName) {
    if (typeof getNewNs.counter == 'undefined') {
        getNewNs.counter = 0;
    }
    getNewNs.counter++;
    const collName = "ns" + getNewNs.counter;
    return [collName, dbName + "." + collName];
}

function checkInStorageCatalog({dbName, collName, type, shardConn}) {
    const query = {name: collName, type};
    assert.eq(1,
              shardConn.getDB(dbName).getCollectionInfos(query).length,
              "Current contents of storage catalog on " + shardConn + ": " +
                  tojson(shardConn.getDB(dbName).getCollectionInfos()) +
                  ", query: " + tojson(query));
}

function checkNotInStorageCatalog({dbName, collName, shardConn}) {
    const query = {name: collName};
    assert.eq(0,
              shardConn.getDB(dbName).getCollectionInfos(query).length,
              "Current contents of storage catalog on " + shardConn + ": " +
                  tojson(shardConn.getDB(dbName).getCollectionInfos()) +
                  ", query: " + tojson(query));
}

function checkInShardingCatalog({ns, shardKey, unique, distributionMode, numChunks, mongosConn}) {
    const configDB = mongosConn.getDB("config");

    // Check the collection entry.
    const collQuery = {};
    collQuery._id = ns;
    collQuery["key." + shardKey] = 1;
    collQuery.unique = unique;
    collQuery.distributionMode = distributionMode;
    assert.neq(null,
               configDB.collections.findOne(collQuery),
               "Current contents of config.collections: " +
                   tojson(configDB.collections.find().toArray()) + ", query: " + tojson(collQuery));

    // Check the chunk entries.
    const chunkQuery = {};
    chunkQuery.ns = ns;
    chunkQuery["min." + shardKey] = {$exists: true};
    chunkQuery["max." + shardKey] = {$exists: true};
    assert.eq(numChunks,
              configDB.chunks.count(chunkQuery),
              "Current contents of config.chunks: " + tojson(configDB.chunks.find().toArray()) +
                  ", query: " + tojson(chunkQuery));
}

function checkNotInShardingCatalog({ns, mongosConn}) {
    const configDB = mongosConn.getDB("config");

    assert.eq(
        null,
        configDB.collections.findOne({_id: ns}),
        "Current contents of config.collections: " + tojson(configDB.collections.find().toArray()));
    assert.eq(0,
              configDB.chunks.count({ns: ns}),
              "Current contents of config.chunks: " + tojson(configDB.chunks.find().toArray()));
}

function setFailpoint(failpointName, conn) {
    assert.commandWorked(conn.adminCommand({
        configureFailPoint: failpointName,
        mode: "alwaysOn",
    }));
}

function unsetFailpoint(failpointName, conn) {
    assert.commandWorked(conn.adminCommand({
        configureFailPoint: failpointName,
        mode: "off",
    }));
}
