/**
 * Helpers for creating and accessing sharding metadata.
 */

import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

export function getShards(db) {
    return db.adminCommand({listShards: 1}).shards;
}

export function getShardNames(db) {
    return getShards(db).map(shard => shard._id);
}

export function getShardHosts(db) {
    return getShards(db).map(shard => shard.host);
}

/**
 * Finds the _id of the primary shard for database 'dbname', e.g., 'test-rs0'
 */
export function getPrimaryShardIdForDatabase(conn, dbname) {
    var x = conn.getDB("config").databases.findOne({_id: dbname});
    if (x) {
        return x.primary;
    }

    throw Error("couldn't find dbname: " + dbname);
}

export function getPrimaryShardNameForDB(db) {
    const config = db.getSiblingDB("config");
    const dbdoc = config.databases.findOne({_id: db.getName()});
    if (dbdoc) {
        return dbdoc.primary;
    } else {
        throw Error("Database " + db.getName() + " not found in config.databases.");
    }
}

export function getShardNamesForCollection(conn, dbName, collName) {
    const ns = dbName + "." + collName;
    const config = conn.getDB("config");
    const collDoc = config.collections.findOne({_id: ns});
    if (!collDoc) {
        // This is an untracked unsharded collection.
        return new Set([getPrimaryShardIdForDatabase(conn, dbName)]);
    }
    const chunkDocs = findChunksUtil.findChunksByNs(config, ns);
    return new Set(chunkDocs.map(doc => doc.shard));
}

export function getNonPrimaryShardName(db) {
    const shardNames = getShardNames(db);
    const primary = getPrimaryShardNameForDB(db);
    for (let i = 0; i < shardNames.length; ++i) {
        if (shardNames[i] !== primary) {
            return shardNames[i];
        }
    }
    return null;
}

/**
 * Returns an array of chunks such that each shard has a chunk range that roughly equally covers
 * [min, max]
 */
export function createChunks(shardNames, shardKey, min, max) {
    let chunks = [];
    let rangeSize = (max - min) / 2;

    if ((max - min + 1) < shardNames.length) {
        throw new Error("[min, max] range is not large enough");
    }

    if (shardNames.length == 1) {
        return [{shard: shardNames[0], min: {[shardKey]: MinKey}, max: {[shardKey]: MaxKey}}];
    }

    for (let i = 0; i < shardNames.length; i++) {
        if (i == 0) {
            chunks.push({min: {[shardKey]: MinKey}, max: {[shardKey]: min}, shard: shardNames[i]});
        } else if (i == shardNames.length - 1) {
            chunks.push({
                min: {[shardKey]: min + (i - 1) * rangeSize},
                max: {[shardKey]: MaxKey},
                shard: shardNames[i]
            });
        } else {
            chunks.push({
                min: {[shardKey]: min + (i - 1) * rangeSize},
                max: {[shardKey]: min + i * (rangeSize)},
                shard: shardNames[i]
            });
        }
    }

    return chunks;
}
