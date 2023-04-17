'use strict';

var CheckShardFilteringMetadataHelpers = (function() {
    function run(mongosConn, nodeConn, shardId, skipCheckShardedCollections = false) {
        function checkDatabase(configDatabasesEntry) {
            // No shard other than the db-primary shard can believe to be the db-primary. Non
            // db-primary shards are allowed to have a stale notion of the dbVersion, as long as
            // they believe they are not primary.

            const dbName = configDatabasesEntry._id;
            print(`CheckShardFilteringMetadata: checking database '${dbName}' on node '${
                nodeConn.host}' of shard '${shardId}'`);

            const nodeMetadata =
                assert.commandWorked(nodeConn.adminCommand({getDatabaseVersion: dbName}));

            // Skip this test if isPrimaryShardForDb is not present. Multiversion incompatible.
            if (nodeMetadata.dbVersion.isPrimaryShardForDb === undefined) {
                return;
            }

            if (nodeMetadata.dbVersion.timestamp === undefined) {
                // Node has no knowledge of the database.
                return;
            }

            assert.eq(
                configDatabasesEntry.primary === shardId,
                nodeMetadata.isPrimaryShardForDb,
                `Unexpected isPrimaryShardForDb for db '${dbName}' on node '${nodeConn.host}'`);

            // If the node is the primary shard for the database, it should know the correct
            // database version.
            if (configDatabasesEntry.primary === shardId) {
                assert.eq(
                    nodeMetadata.dbVersion.uuid,
                    configDatabasesEntry.version.uuid,
                    `Unexpected dbVersion.uuid for db '${dbName}' on node '${nodeConn.host}'`);
                assert.eq(timestampCmp(nodeMetadata.dbVersion.timestamp,
                                       configDatabasesEntry.version.timestamp),
                          0,
                          `Unexpected dbVersion timestamp for db '${dbName}' on node '${
                              nodeConn.host}'. Found '${
                              tojson(nodeMetadata.dbVersion.timestamp)}'; expected '${
                              tojson(configDatabasesEntry.version.timestamp)}'`);
                assert.eq(
                    nodeMetadata.dbVersion.lastMod,
                    configDatabasesEntry.version.lastMod,
                    `Unexpected dbVersion lastMod for db '${dbName}' on node '${nodeConn.host}'`);
            }

            print(`CheckShardFilteringMetadata: Database '${dbName}' on '${nodeConn.host}' OK`);
        }

        function checkShardedCollection(coll, nodeShardingState) {
            const ns = coll._id;
            print(`CheckShardFilteringMetadata: checking collection '${ns} ' on node '${
                nodeConn.host}' of shard '${shardId}'`);

            const configDB = mongosConn.getDB('config');

            const highestChunkOnShard = configDB.chunks.find({uuid: coll.uuid, shard: shardId})
                                            .sort({lastmod: -1})
                                            .limit(1)
                                            .toArray()[0];

            const expectedShardVersion =
                highestChunkOnShard ? highestChunkOnShard.lastmod : Timestamp(0, 0);
            const expectedTimestamp = coll.timestamp;

            const collectionMetadataOnNode = nodeShardingState.versions[ns];
            if (collectionMetadataOnNode === undefined) {
                // Shards are not authoritative. It is okay that they don't know their filtering
                // info.
                return;
            }

            if (collectionMetadataOnNode.timestamp === undefined) {
                // Versions earlier than v6.3 did not report the timestamp on shardingState command
                // (SERVER-70790). This early exit can be removed after v6.0 is no longer tested in
                // multiversion suites.
                return;
            }

            if (timestampCmp(collectionMetadataOnNode.timestamp, Timestamp(0, 0)) === 0) {
                // The metadata reflects an unsharded collection. It is okay for a node to have this
                // stale metadata, as long as the node knows the correct dbVersion.
                return;
            }

            // If the node knows its filtering info, then assert that it is correct.
            assert.eq(timestampCmp(collectionMetadataOnNode.timestamp, expectedTimestamp),
                      0,
                      `Unexpected timestamp for ns '${ns}' on node '${nodeConn.host}'. Found '${
                          tojson(collectionMetadataOnNode.timestamp)}', expected '${
                          tojson(expectedTimestamp)}'`);
            // Only check the major version because some operations (such as resharding or
            // setAllowMigrations) bump the minor version without the shards knowing. This does not
            // affect placement, so it is okay.
            assert.eq(collectionMetadataOnNode.placementVersion.t,
                      expectedShardVersion.t,
                      `Unexpected shardVersion for ns '${ns}' on node '${nodeConn.host}'`);

            print(`CheckShardFilteringMetadata: ns '${ns}' on '${nodeConn.host}' OK`);
        }

        const configDB = mongosConn.getDB('config');

        // Check shards know correct database versions.
        configDB.databases.find().forEach(configDatabasesEntry => {
            checkDatabase(configDatabasesEntry);
        });

        // Check that shards have correct filtering metadata for sharded collections.
        if (!skipCheckShardedCollections) {
            const nodeShardingState = nodeConn.adminCommand({shardingState: 1});
            configDB.collections.find().forEach(coll => {
                checkShardedCollection(coll, nodeShardingState);
            });
        }

        print("CheckShardFilteringMetadata: finished");
    }

    return {
        run: run,
    };
})();
