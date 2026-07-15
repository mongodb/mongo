/*
 * Utilities for shard versioning testing.
 */
import {ChunkHelper} from "jstests/concurrency/fsm_workload_helpers/cluster_scalability/chunks.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

export var ShardVersioningUtil = (function () {
    /*
     * Shard version indicating that shard version checking must be skipped.
     */
    const kIgnoredShardVersion = {
        e: ObjectId("00000000ffffffffffffffff"),
        t: Timestamp(Math.pow(2, 32) - 1, Math.pow(2, 32) - 1),
        v: Timestamp(0, 0),
    };

    /*
     * Shard version representing an UNTRACKED collection.
     */
    const kUntrackedShardVersion = {
        e: ObjectId("000000000000000000000000"),
        t: Timestamp(0, 0),
        v: Timestamp(0, 0),
    };

    /*
     * Returns the metadata for the collection in the shard's catalog cache.
     */
    let getMetadataOnShard = function (shard, ns, waitForRefresh = false) {
        if (waitForRefresh) {
            // Wait for the last routing table cache refresh to be persisted on disk
            if (!FeatureFlagUtil.isPresentAndEnabled(shard, "AuthoritativeShardsCRUD")) {
                assert.commandWorked(shard.adminCommand({_flushRoutingTableCacheUpdates: ns}));
            }
        }

        let res = assert.commandWorked(
            shard.adminCommand({getShardVersion: ns, fullMetadata: true}),
        );
        return res.metadata;
    };

    /*
     * Returns the shard version of a collection on the given shard.
     */
    let getShardVersion = function (shard, ns, waitForRefresh = false) {
        let res = getMetadataOnShard(shard, ns, waitForRefresh);
        return {e: res.shardVersionEpoch, t: res.shardVersionTimestamp, v: res.shardVersion};
    };

    /*
     * Returns the shardingStatistics section of the given node's serverStatus, or an empty object
     * if it is absent.
     */
    let getShardingStatistics = function (conn) {
        return assert.commandWorked(conn.adminCommand({serverStatus: 1})).shardingStatistics || {};
    };

    /*
     * Returns the number of StaleConfig errors the given node has observed while acting as a router
     * (i.e. received back from a shard and then refreshed for), read from
     * serverStatus().shardingStatistics.catalogCache.countStaleConfigErrors. Works for a mongos as
     * well as for the config server or a shard acting as a router.
     */
    let getRouterStaleConfigErrorCount = function (conn) {
        return Number((getShardingStatistics(conn).catalogCache || {}).countStaleConfigErrors || 0);
    };

    /*
     * Returns the number of StaleConfig errors the given shard has thrown while rejecting
     * stale-versioned requests, read from the top-level
     * serverStatus().shardingStatistics.countStaleConfigErrors. This is distinct from the router-side
     * catalogCache counter read by getRouterStaleConfigErrorCount.
     */
    let getShardStaleConfigErrorCount = function (conn) {
        return Number(getShardingStatistics(conn).countStaleConfigErrors || 0);
    };

    /*
     * Asserts that the collection version for the collection in the shard's catalog cache
     * is equal to the given collection version.
     */
    let assertCollectionVersionEquals = function (shard, ns, collectionVersion) {
        assert.eq(getMetadataOnShard(shard, ns).collVersion, collectionVersion);
    };

    /*
     * Asserts that the collection version for the collection in the shard's catalog cache
     * is older than the given collection version.
     */
    let assertCollectionVersionOlderThan = function (shard, ns, collectionVersion) {
        let shardCollectionVersion = getMetadataOnShard(shard, ns).collVersion;
        if (shardCollectionVersion != undefined) {
            assert.lt(shardCollectionVersion.t, collectionVersion.t);
        }
    };

    /*
     * Asserts that the shard version of the shard in its catalog cache is equal to the
     * given shard version.
     */
    let assertShardVersionEquals = function (shard, ns, shardVersion) {
        assert.eq(getMetadataOnShard(shard, ns).shardVersion, shardVersion);
    };

    /*
     * Moves the chunk that matches the given query to toShard. Forces the recipient to skip the
     * metadata refresh post-migration commit.
     *
     * TODO (SERVER-129875): Analyze all usages of this function and determine if we need to add a
     * similar failpoint for the donor when the shard is authoritative.
     */
    let moveChunkNotRefreshRecipient = function (mongos, ns, fromShard, toShard, findQuery) {
        let failPoint = configureFailPoint(toShard, "migrationRecipientFailPostCommitRefresh");

        assert.commandWorked(
            mongos.adminCommand({
                moveChunk: ns,
                find: findQuery,
                to: toShard.shardName,
                _waitForDelete: true,
            }),
        );

        failPoint.off();
    };

    /*
     * Reproduces a stale-routing scenario by leaving a *router* stale rather than a shard. A shard
     * can never be ignorant of its own filtering metadata under authoritative shards (the migration
     * installs it directly on both shards at commit), so the only remaining legitimate source of
     * StaleConfig is a router whose cached routing table lags. This approach works identically in
     * both the legacy and authoritative models.
     *
     * Given two routers, it:
     *   1. Primes staleRouter's routing cache for `ns` (so it holds a routing table that the
     *      migration will make stale).
     *   2. Moves the chunk with the given `bounds` ([min, max]) to `toShard` through migrateRouter,
     *      refreshing that router and the shards but leaving staleRouter pointing at the old owning
     *      shard.
     *   3. Runs `runStaleOperation(staleRouter)`, which routes by the stale cache, receives a
     *      StaleConfig from the now-authoritative shard, then transparently refreshes and retries.
     *
     * Asserts that at least one StaleConfig error was observed and retried. The counters of the
     * stale router and of every shard are watched, which covers both retry locations (see below).
     * This can miss nothing relevant, but it may over-count if unrelated background work raises a
     * StaleConfig in the sampling window; such a false negative (the assertion passing when the
     * operation itself did not go stale) is acceptable, whereas a false failure is not possible.
     *
     * Returns whatever `runStaleOperation` returns, so callers can make further assertions on it.
     */
    let runOperationOnStaleRouterAfterMoveChunk = function ({
        migrateRouter,
        staleRouter,
        ns,
        toShard,
        bounds,
        runStaleOperation,
    }) {
        const [dbName, ...collNameParts] = ns.split(".");
        const collName = collNameParts.join(".");
        const shardConns = staleRouter
            .getDB("config")
            .shards.find()
            .toArray()
            .map((shardDoc) => new Mongo(shardDoc.host));
        // A StaleConfig is always thrown by a shard that receives a stale-versioned request, which
        // records it in the shard's top-level shardingStatistics.countStaleConfigErrors. The router
        // that then refreshes records it in catalogCache.countStaleConfigErrors. CRUD commands are
        // retried on the router that issued them, whereas DDL commands (e.g. dropIndexes) are routed
        // to a shard-side coordinator that retries there. Summing the stale router's router-side
        // counter and every shard's shard-side counter covers both cases.
        const totalStaleConfig = () =>
            shardConns.reduce(
                (sum, shardConn) => sum + getShardStaleConfigErrorCount(shardConn),
                getRouterStaleConfigErrorCount(staleRouter),
            );

        // Prime the stale router's routing cache so it holds a routing table that the upcoming
        // migration will make stale.
        assert.commandWorked(staleRouter.getCollection(ns).runCommand("find", {filter: bounds[0]}));

        // Perform the migration through the other router. This refreshes migrateRouter and the
        // shards but leaves staleRouter's cache pointing at the old owning shard.
        ChunkHelper.moveChunk(
            migrateRouter.getDB(dbName),
            collName,
            bounds,
            toShard.shardName,
            true /* waitForDelete */,
        );

        // Sample the counters immediately around the operation so the increment is attributable to
        // the stale routing rather than to the migration itself.
        const staleConfigCountBefore = totalStaleConfig();

        // The stale router routes by its old cache, a shard rejects the stale version with a
        // StaleConfig, and the operation refreshes and retries.
        const result = runStaleOperation(staleRouter);

        assert.gt(
            totalStaleConfig(),
            staleConfigCountBefore,
            "expected a StaleConfig error to be observed and retried",
        );

        return result;
    };

    const getDbVersion = function (mongos, dbName) {
        const version = mongos.getDB("config")["databases"].findOne({_id: dbName}).version;
        // Explicitly make lastMod an int so that the server doesn't complain
        // it's a double if you pass a dbVersion to a parallel shell.
        return {...version, lastMod: NumberInt(version.lastMod)};
    };

    return {
        kIgnoredShardVersion,
        kUntrackedShardVersion,
        getMetadataOnShard,
        getShardVersion,
        getRouterStaleConfigErrorCount,
        getShardStaleConfigErrorCount,
        assertCollectionVersionEquals,
        assertCollectionVersionOlderThan,
        assertShardVersionEquals,
        moveChunkNotRefreshRecipient,
        runOperationOnStaleRouterAfterMoveChunk,
        getDbVersion,
    };
})();
