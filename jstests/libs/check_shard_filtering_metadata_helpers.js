import {RetryableWritesUtil} from "jstests/libs/retryable_writes_util.js";

export var CheckShardFilteringMetadataHelpers = (function () {
    function run(mongosConn, nodeConn, shardId, skipCheckShardedCollections = false) {
        function checkDatabase(configDatabasesEntry) {
            // No shard other than the db-primary shard can believe to be the db-primary. Non
            // db-primary shards are allowed to have a stale notion of the dbVersion, as long as
            // they believe they are not primary.

            const dbName = configDatabasesEntry._id;
            jsTest.log.info(
                `CheckShardFilteringMetadata: checking database '${dbName}' on node '${
                    nodeConn.host
                }' of shard '${shardId}'`,
            );

            const nodeMetadata = assert.commandWorked(nodeConn.adminCommand({getDatabaseVersion: dbName}));

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
                `Unexpected isPrimaryShardForDb for db '${dbName}' on node '${nodeConn.host}'`,
            );

            // If the node is the primary shard for the database, it should know the correct
            // database version.
            if (configDatabasesEntry.primary === shardId) {
                assert.eq(
                    nodeMetadata.dbVersion.uuid,
                    configDatabasesEntry.version.uuid,
                    `Unexpected dbVersion.uuid for db '${dbName}' on node '${nodeConn.host}'`,
                );
                assert.eq(
                    timestampCmp(nodeMetadata.dbVersion.timestamp, configDatabasesEntry.version.timestamp),
                    0,
                    `Unexpected dbVersion timestamp for db '${dbName}' on node '${nodeConn.host}'. Found '${tojson(
                        nodeMetadata.dbVersion.timestamp,
                    )}'; expected '${tojson(configDatabasesEntry.version.timestamp)}'`,
                );
                assert.eq(
                    nodeMetadata.dbVersion.lastMod,
                    configDatabasesEntry.version.lastMod,
                    `Unexpected dbVersion lastMod for db '${dbName}' on node '${nodeConn.host}'`,
                );
            }

            jsTest.log.info(`CheckShardFilteringMetadata: Database '${dbName}' on '${nodeConn.host}' OK`);
        }

        function getPrimaryShardForDB(dbName) {
            if (dbName == "config") {
                return "config";
            }

            const configDB = mongosConn.getDB("config");

            const dbEntry = configDB.databases.findOne({_id: dbName});
            assert(dbEntry, `Couldn't find database '${dbName}' in 'config.databases'`);
            assert(
                dbEntry.primary,
                `Database entry for db '${dbName}' does not contain primary shard: ${tojson(dbEntry)}`,
            );
            return dbEntry.primary;
        }

        function checkShardedCollection(coll, nodeShardingState) {
            const ns = coll._id;
            jsTest.log.info(
                `CheckShardFilteringMetadata: checking collection '${ns} ' on node '${
                    nodeConn.host
                }' of shard '${shardId}'`,
            );

            const configDB = mongosConn.getDB("config");

            const dbName = mongosConn.getCollection(ns).getDB().getName();
            const primaryShardId = getPrimaryShardForDB(dbName);
            const highestChunkOnShard = configDB.chunks
                .find({uuid: coll.uuid, shard: shardId})
                .sort({lastmod: -1})
                .limit(1)
                .toArray()[0];

            const expectedTimestamp = coll.timestamp;

            const collectionMetadataOnNode = nodeShardingState.versions[ns];
            if (collectionMetadataOnNode === undefined) {
                // Shards are not authoritative. It is okay that they don't know their filtering
                // info.
                return;
            }

            if (shardId != getPrimaryShardForDB(dbName) && !highestChunkOnShard) {
                // The shard is neither primary for database nor owns some chunks for this
                // collection.
                // In this case the shard is allow to have a stale/wrong collection
                // metadata as long as it has the correct db version.
                return;
            }

            // Check that timestamp is correct
            assert.eq(
                timestampCmp(collectionMetadataOnNode.timestamp, expectedTimestamp),
                0,
                `Unexpected timestamp for ns '${ns}' on node '${nodeConn.host}'. Found '${tojson(
                    collectionMetadataOnNode.timestamp,
                )}', expected '${tojson(expectedTimestamp)}'`,
            );

            // Check that placement version is correct
            const expectedShardVersion = highestChunkOnShard ? highestChunkOnShard.lastmod : Timestamp(0, 0);

            // Only check the major version because some operations (such as resharding or
            // setAllowMigrations) bump the minor version without the shards knowing. This does not
            // affect placement, so it is okay.
            assert.eq(
                collectionMetadataOnNode.placementVersion.t,
                expectedShardVersion.t,
                `Unexpected shardVersion for ns '${ns}' on node '${nodeConn.host}'`,
            );

            jsTest.log.info(`CheckShardFilteringMetadata: ns '${ns}' on '${nodeConn.host}' OK`);
        }

        const configDB = mongosConn.getDB("config");

        // Check shards know correct database versions.
        configDB.databases.find().forEach((configDatabasesEntry) => {
            checkDatabase(configDatabasesEntry);
        });

        // Check that shards have correct filtering metadata for sharded collections.
        if (!skipCheckShardedCollections) {
            const nodeShardingState = nodeConn.adminCommand({shardingState: 1});
            configDB.collections.find().forEach((coll) => {
                checkShardedCollection(coll, nodeShardingState);
            });
        }

        jsTest.log.info("CheckShardFilteringMetadata: finished");
    }

    function isTransientError(e) {
        return (
            ErrorCodes.isRetriableError(e.code) ||
            ErrorCodes.isInterruption(e.code) ||
            ErrorCodes.isNetworkTimeoutError(e.code) ||
            isNetworkError(e) ||
            e.code === ErrorCodes.FailedToSatisfyReadPreference ||
            RetryableWritesUtil.isFailedToSatisfyPrimaryReadPreferenceError(e)
        );
    }

    return {
        run: run,
        isTransientError: isTransientError,
    };
})();
