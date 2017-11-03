/**
 * Load this file when starting a sharded cluster to provide a callback to check that collection
 * UUIDs are consistent across shards and the config server.
 *
 * To disable this check on a specific test, add the following to the JS file:
 *
 *     TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
 *
 * Common test actions that can cause the UUID check to fail are:
 * - Causing a shard not to have a primary. The UUID check must run commands against primaries.
 * - Restarting shard replica sets / primaries. The UUID consistency check uses shard primary
 *   connections cached in ShardingTest._connections, which may become out of date if the primary
 *   changes and the shell doesn't detect the new primary before this check occurs.
 * - No config primary. The check queries the config collections.
 * - Blackholing mongos from the config servers. The UUID check uses the mongos to query the config.
 */

"use strict";

ShardingTest.prototype.checkUUIDsConsistentAcrossCluster = function() {
    if (jsTest.options().skipCheckingUUIDsConsistentAcrossCluster) {
        print("Skipping checking UUID consistency across the cluster");
        return;
    }
    print("Checking UUID consistency across the cluster");

    function parseNs(dbDotColl) {
        assert.gt(dbDotColl.indexOf('.'),
                  0,
                  "expected " + dbDotColl + " to represent a full collection name");
        const dbName = dbDotColl.substring(0, dbDotColl.indexOf('.'));
        const collName = dbDotColl.substring(dbDotColl.indexOf('.') + 1, dbDotColl.length);
        return [dbName, collName];
    }

    try {
        // Read from config.collections, config.shards, and config.chunks to construct a picture
        // of which shards own data for which collections, and what the UUIDs for those collections
        // are.
        let authoritativeCollMetadataArr =
            this.s.getDB("config")
                .chunks
                .aggregate([
                    {
                      $lookup: {
                          from: "shards",
                          localField: "shard",
                          foreignField: "_id",
                          as: "shardHost"
                      }
                    },
                    {$unwind: "$shardHost"},
                    {$group: {_id: "$ns", shardConnStrings: {$addToSet: "$shardHost.host"}}},
                    {
                      $lookup: {
                          from: "collections",
                          localField: "_id",
                          foreignField: "_id",
                          as: "collInfo"
                      }
                    },
                    {$unwind: "$collInfo"}
                ])
                .toArray();

        print("Aggregated authoritative metadata on config server for all sharded collections: " +
              tojson(authoritativeCollMetadataArr));

        // The ShardingTest object maintains a connection to each shard in its _connections array,
        // where each connection is tagged with the shard's connection string in a 'host' field.
        // Create a reverse mapping of connection string to connection to efficiently retrieve a
        // connection by connection string.
        let shardConnStringToConn = {};
        this._connections.forEach(function(conn) {
            shardConnStringToConn[conn.host] = conn;
        });

        for (let authoritativeCollMetadata of authoritativeCollMetadataArr) {
            const[dbName, collName] = parseNs(authoritativeCollMetadata._id);

            for (let shardConnString of authoritativeCollMetadata.shardConnStrings) {
                // A connection the shard may not be cached in ShardingTest if the shard was added
                // manually to the cluster by the test.
                if (!(shardConnStringToConn.hasOwnProperty(shardConnString))) {
                    print("Creating connection to manually added shard: " + shardConnString);
                    shardConnStringToConn[shardConnString] = new Mongo(shardConnString);
                }
                let shardConn = shardConnStringToConn[shardConnString];

                print("running listCollections against " + shardConn +
                      " to check UUID consistency for " + authoritativeCollMetadata._id);
                const actualCollMetadata =
                    shardConn.getDB(dbName).getCollectionInfos({name: collName})[0];
                assert.eq(authoritativeCollMetadata.collInfo.uuid,
                          actualCollMetadata.info.uuid,
                          "authoritative collection info on config server: " +
                              tojson(authoritativeCollMetadata.collInfo) +
                              ", actual collection info on shard " + shardConnString + ": " +
                              tojson(actualCollMetadata));

                print("checking " + shardConn + " config.collections" +
                      " for UUID consistency with " + authoritativeCollMetadata._id);
                const configDB = shardConn.getDB('config');
                const actualConfigMetadata =
                    configDB.cache.collections.find({"_id": dbName + "." + collName})[0];
                assert.eq(authoritativeCollMetadata.collInfo.uuid,
                          actualConfigMetadata.uuid,
                          "authoritative collection info on config server: " +
                              tojson(authoritativeCollMetadata.collInfo) +
                              ", actual config info on shard " + shardConnString + ": " +
                              tojson(actualConfigMetadata));
            }
        }
    } catch (e) {
        if (e.message.indexOf("Unauthorized") < 0) {
            throw e;
        }
        print("ignoring exception " + tojson(e) +
              " while checking UUID consistency across cluster");
    }
};
