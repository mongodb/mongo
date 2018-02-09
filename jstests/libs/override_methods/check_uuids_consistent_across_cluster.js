/**
 * Provides a hook to check that shards' storage catalogs and catalog caches are consistent
 * with the sharding catalog on the config server.
 *
 * The hook currently checks that: if the sharding catalog says a shard owns chunks for a sharded
 * collection, then the shard has an entry for the collection
 * - in its storage catalog, with the same UUID as the collection has in the sharding catalog
 * - in its catalog cache, with the same UUID as the collection has in the sharding catalog
 *
 * TODO (SERVER-33252): extend the hook to add consistency checks for databases
 * TODO (SERVER-33253): extend the hook to add consistency checks for collection indexes and options
 */
"use strict";

ShardingTest.prototype.checkUUIDsConsistentAcrossCluster = function() {
    if (jsTest.options().skipCheckingUUIDsConsistentAcrossCluster) {
        // A test may want to skip the consistency checks for a few reasons:
        // 1)  The checks are performed against shard and config primaries, and the connections
        //     cached on ShardingTest are used. So, tests that end with a different (or no) shard or
        //     config primary should skip the checks.
        // 2) The sharding catalog is read from the config server via mongos, so tests that cause
        //    the config primary to be unreachable from mongos should skip the checks.
        print(
            "Skipping checking consistency of the sharding catalog with shards' storage catalogs and catalog caches");
        return;
    }

    if (jsTest.options().skipCheckingCatalogCacheConsistencyWithShardingCatalog) {
        // When a shard takes or loses ownership of a chunk (through shardCollection, moveChunk, or
        // dropCollection), a best-effort is made to make the shard refresh its routing table cache.
        // But since sharding catalog changes are not transactional, it's possible the shard's
        // catalog cache will be stale. A test or suite that induces stepdowns or otherwise makes it
        // likely that this "best-effort" will fail should skip checks for only the catalog caches.
        print(
            "Checking consistency of the sharding catalog with shards' storage catalogs, but not with shards' catalog caches");
    } else {
        print(
            "Checking consistency of the sharding catalog with shards' storage catalogs and catalog caches");
    }

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
            const ns = authoritativeCollMetadata._id;
            const[dbName, collName] = parseNs(ns);

            for (let shardConnString of authoritativeCollMetadata.shardConnStrings) {
                // A connection the shard may not be cached in ShardingTest if the shard was added
                // manually to the cluster by the test.
                if (!(shardConnStringToConn.hasOwnProperty(shardConnString))) {
                    print("Creating connection to manually added shard: " + shardConnString);
                    shardConnStringToConn[shardConnString] = new Mongo(shardConnString);
                }
                let shardConn = shardConnStringToConn[shardConnString];

                print("Checking that the UUID for " + ns + " returned by listCollections on " +
                      shardConn +
                      " is consistent with the UUID in config.collections on the config server");

                const actualCollMetadata =
                    shardConn.getDB(dbName).getCollectionInfos({name: collName})[0];
                assert.eq(authoritativeCollMetadata.collInfo.uuid,
                          actualCollMetadata.info.uuid,
                          "authoritative collection info on config server: " +
                              tojson(authoritativeCollMetadata.collInfo) +
                              ", actual collection info on shard " + shardConnString + ": " +
                              tojson(actualCollMetadata));

                if (!jsTest.options().skipCheckingCatalogCacheConsistencyWithShardingCatalog) {
                    print(
                        "Checking that the UUID for " + ns + " in config.cache.collections on " +
                        shardConn +
                        " is consistent with the UUID in config.collections on the config server");

                    // Wait for the shard to finish writing its last refresh to disk.
                    assert.commandWorked(shardConn.adminCommand(
                        {_flushRoutingTableCacheUpdates: ns, syncFromConfig: false}));

                    const actualConfigMetadata =
                        shardConn.getDB("config").getCollection("cache.collections").find({
                            "_id": ns
                        })[0];
                    assert.eq(authoritativeCollMetadata.collInfo.uuid,
                              actualConfigMetadata.uuid,
                              "authoritative collection info on config server: " +
                                  tojson(authoritativeCollMetadata.collInfo) +
                                  ", actual config info on shard " + shardConnString + ": " +
                                  tojson(actualConfigMetadata));
                }
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
