/*
 * Utilities for shard versioning testing.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";

export var ShardVersioningUtil = (function() {
    /*
     * Shard version indicating that shard version checking must be skipped.
     */
    const kIgnoredShardVersion = {
        e: ObjectId("00000000ffffffffffffffff"),
        t: Timestamp(Math.pow(2, 32) - 1, Math.pow(2, 32) - 1),
        v: Timestamp(0, 0)
    };

    /*
     * Shard version representing an UNTRACKED collection.
     */
    const kUntrackedShardVersion = {
        e: ObjectId("000000000000000000000000"),
        t: Timestamp(0, 0),
        v: Timestamp(0, 0)
    };

    /*
     * Returns the metadata for the collection in the shard's catalog cache.
     */
    let getMetadataOnShard = function(shard, ns, waitForRefresh = false) {
        if (waitForRefresh) {
            // Wait for the last routing table cache refresh to be persisted on disk
            assert.commandWorked(shard.adminCommand({_flushRoutingTableCacheUpdates: ns}));
        }

        let res =
            assert.commandWorked(shard.adminCommand({getShardVersion: ns, fullMetadata: true}));
        return res.metadata;
    };

    /*
     * Returns the shard version of a collection on the given shard.
     */
    let getShardVersion = function(shard, ns, waitForRefresh = false) {
        let res = getMetadataOnShard(shard, ns, waitForRefresh);
        return {e: res.shardVersionEpoch, t: res.shardVersionTimestamp, v: res.shardVersion};
    };

    /*
     * Asserts that the collection version for the collection in the shard's catalog cache
     * is equal to the given collection version.
     */
    let assertCollectionVersionEquals = function(shard, ns, collectionVersion) {
        assert.eq(getMetadataOnShard(shard, ns).collVersion, collectionVersion);
    };

    /*
     * Asserts that the collection version for the collection in the shard's catalog cache
     * is older than the given collection version.
     */
    let assertCollectionVersionOlderThan = function(shard, ns, collectionVersion) {
        let shardCollectionVersion = getMetadataOnShard(shard, ns).collVersion;
        if (shardCollectionVersion != undefined) {
            assert.lt(shardCollectionVersion.t, collectionVersion.t);
        }
    };

    /*
     * Asserts that the shard version of the shard in its catalog cache is equal to the
     * given shard version.
     */
    let assertShardVersionEquals = function(shard, ns, shardVersion) {
        assert.eq(getMetadataOnShard(shard, ns).shardVersion, shardVersion);
    };

    /*
     * Moves the chunk that matches the given query to toShard. Forces the recipient to skip the
     * metadata refresh post-migration commit.
     */
    let moveChunkNotRefreshRecipient = function(mongos, ns, fromShard, toShard, findQuery) {
        let failPoint = configureFailPoint(toShard, "migrationRecipientFailPostCommitRefresh");

        assert.commandWorked(mongos.adminCommand(
            {moveChunk: ns, find: findQuery, to: toShard.shardName, _waitForDelete: true}));

        failPoint.off();
    };

    const getDbVersion = function(mongos, dbName) {
        const version = mongos.getDB('config')['databases'].findOne({_id: dbName}).version;
        // Explicitly make lastMod an int so that the server doesn't complain
        // it's a double if you pass a dbVersion to a parallel shell.
        return {...version, lastMod: NumberInt(version.lastMod)};
    };

    return {
        kIgnoredShardVersion,
        kUntrackedShardVersion,
        getMetadataOnShard,
        getShardVersion,
        assertCollectionVersionEquals,
        assertCollectionVersionOlderThan,
        assertShardVersionEquals,
        moveChunkNotRefreshRecipient,
        getDbVersion
    };
})();
