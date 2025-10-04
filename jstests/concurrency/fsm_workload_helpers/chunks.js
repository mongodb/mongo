/**
 * Provides wrapper functions that perform exponential backoff and allow for
 * acceptable errors to be returned from mergeChunks, moveChunk, and splitChunk
 * commands.
 *
 * Also provides functions to help perform assertions about the state of chunks.
 *
 * Intended for use by workloads testing sharding (i.e., workloads starting with 'sharded_').
 */

import {isMongod, isMongodConfigsvr, isMongos} from "jstests/concurrency/fsm_workload_helpers/server_types.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

export var ChunkHelper = (function () {
    // exponential backoff
    function getNextBackoffSleep(curSleep) {
        const MAX_BACKOFF_SLEEP = 5000; // milliseconds

        curSleep *= 2;
        return Math.min(curSleep, MAX_BACKOFF_SLEEP);
    }

    function runCommandWithRetries(db, cmd, didAcceptableErrorOccurFn) {
        const INITIAL_BACKOFF_SLEEP = 500; // milliseconds
        const MAX_RETRIES = 5;

        let res;
        let retries = 0;
        let backoffSleep = INITIAL_BACKOFF_SLEEP;
        while (retries < MAX_RETRIES) {
            retries++;
            res = db.adminCommand(cmd);
            // If the command worked, exit the loop early.
            if (res.ok) {
                return res;
            }
            // Assert command worked or acceptable error occurred.
            if (didAcceptableErrorOccurFn(res)) {
                // When an acceptable error occurs, sleep and then retry.
                sleep(backoffSleep);
                backoffSleep = getNextBackoffSleep(backoffSleep);
                continue;
            }

            // Throw an exception if the command errored for any other reason.
            assert.commandWorked(res, cmd);
        }

        return res;
    }

    function splitChunkAt(db, collName, middle) {
        let cmd = {split: db[collName].getFullName(), middle: middle};
        return runCommandWithRetries(db, cmd, (res) => res.code === ErrorCodes.LockBusy);
    }

    function splitChunkAtPoint(db, collName, splitPoint) {
        return splitChunkAt(db, collName, {_id: splitPoint});
    }

    function splitChunkWithBounds(db, collName, bounds) {
        let cmd = {split: db[collName].getFullName(), bounds: bounds};
        return runCommandWithRetries(db, cmd, (res) => res.code === ErrorCodes.LockBusy);
    }

    function moveChunk(db, collName, bounds, toShard, waitForDelete, secondaryThrottle) {
        let cmd = {
            moveChunk: db[collName].getFullName(),
            bounds: bounds,
            to: toShard,
        };

        if (waitForDelete != null) {
            cmd._waitForDelete = waitForDelete;
        }

        // Using _secondaryThrottle adds coverage for additional waits for write concern on the
        // recipient during cloning.
        if (secondaryThrottle != null) {
            cmd._secondaryThrottle = secondaryThrottle;
            cmd.writeConcern = {w: "majority"}; // _secondaryThrottle requires a write concern.
        }

        const runningWithStepdowns = TestData.runningWithConfigStepdowns || TestData.runningWithShardStepdowns;
        const isSlowBuild = () => {
            // Consider this a slow build on TSAN or if we're fuzzing mongod configs, which
            // can intentionally slow the server.
            return TestData.fuzzMongodConfigs || db.getServerBuildInfo().isThreadSanitizerActive();
        };

        return runCommandWithRetries(
            db,
            cmd,
            (res) =>
                res.code === ErrorCodes.ConflictingOperationInProgress ||
                res.code === ErrorCodes.ChunkRangeCleanupPending ||
                res.code === ErrorCodes.LockTimeout ||
                // The chunk migration has surely been aborted if the startCommit of the
                // procedure was interrupted by a stepdown.
                ((runningWithStepdowns || isSlowBuild()) &&
                    res.code === ErrorCodes.CommandFailed &&
                    res.errmsg.includes("startCommit")) ||
                // The chunk migration has surely been aborted if the recipient shard didn't
                // believe there was an active chunk migration.
                (runningWithStepdowns &&
                    res.code === ErrorCodes.OperationFailed &&
                    res.errmsg.includes("NotYetInitialized")) ||
                // The chunk migration has surely been aborted if there was another active
                // chunk migration on the donor.
                (runningWithStepdowns &&
                    res.code === ErrorCodes.OperationFailed &&
                    res.errmsg.includes("does not match active session id")) ||
                // A stepdown may cause the collection's lock to become temporarily unreleasable
                // and cause the chunk migration to timeout.  The migration may still succeed
                // after the lock's lease expires.
                (runningWithStepdowns && res.code === ErrorCodes.LockBusy) ||
                // Slow builds can result in a failure when waiting for write concern in certain
                // migration phases.
                (isSlowBuild() &&
                    res.code === ErrorCodes.OperationFailed &&
                    res.errmsg.includes("WriteConcernTimeout")),
        );
    }

    function mergeChunks(db, collName, bounds) {
        let cmd = {mergeChunks: db[collName].getFullName(), bounds: bounds};
        return runCommandWithRetries(db, cmd, (res) => res.code === ErrorCodes.LockBusy);
    }

    // Take a set of connections to a shard (replica set or standalone mongod),
    // or a set of connections to the config servers, and return a connection
    // to any node in the set for which isWritablePrimary is true.
    function getPrimary(connArr) {
        const kDefaultTimeoutMS = 10 * 60 * 1000; // 10 minutes.
        assert(Array.isArray(connArr), "Expected an array but got " + tojson(connArr));

        let primary = null;
        assert.soon(
            () => {
                for (let conn of connArr) {
                    assert(isMongod(conn.getDB("admin")), tojson(conn) + " is not to a mongod");
                    let res = conn.adminCommand({hello: 1});
                    assert.commandWorked(res);

                    if (res.isWritablePrimary) {
                        primary = conn;
                        return primary;
                    }
                }
            },
            "Finding primary timed out",
            kDefaultTimeoutMS,
        );

        return primary;
    }

    // Take a set of mongos connections to a sharded cluster and return a
    // random connection.
    function getRandomMongos(connArr) {
        assert(Array.isArray(connArr), "Expected an array but got " + tojson(connArr));
        let conn = connArr[Random.randInt(connArr.length)];
        assert(isMongos(conn.getDB("admin")), tojson(conn) + " is not to a mongos");
        return conn;
    }

    // Intended for use on mongos connections only.
    // Return all shards containing documents in [lower, upper).
    function getShardsForRange(conn, collName, lower, upper) {
        assert(isMongos(conn.getDB("admin")), tojson(conn) + " is not to a mongos");
        let adminDB = conn.getDB("admin");
        let shardVersion = adminDB.runCommand({getShardVersion: collName, fullMetadata: true});
        assert.commandWorked(shardVersion);
        // As noted in SERVER-20768, doing a range query with { $lt : X },  where
        // X is the _upper bound_ of a chunk,  incorrectly targets the shard whose
        // _lower bound_ is X. Therefore, if upper !== MaxKey, we use a workaround
        // to ensure that only the shard whose lower bound = X is targeted.
        let query;
        if (upper === MaxKey) {
            query = {$and: [{_id: {$gte: lower}}, {_id: {$lt: upper}}]};
        } else {
            query = {$and: [{_id: {$gte: lower}}, {_id: {$lte: upper - 1}}]};
        }
        let res = conn.getCollection(collName).find(query).explain();
        assert.commandWorked(res);
        assert.gt(res.queryPlanner.winningPlan.shards.length, 0, "Explain did not have shards key.");

        let shards = res.queryPlanner.winningPlan.shards.map((shard) => shard.shardName);
        return {shards: shards, explain: res, query: query, shardVersion: shardVersion};
    }

    function itcount(collection, query) {
        // We set a large batch size and project out all of the fields in order to greatly reduce
        // the likelihood a cursor would actually be returned. This is acceptable because we're only
        // interested in how many documents there were and not any of their contents. The
        // network_error_and_txn_override.js override would throw an exception if we attempted to
        // use the getMore command.
        return collection.find(query, {_id: 0, nonExistingField: 1}).batchSize(1e6).itcount();
    }

    // Return the number of docs in [lower, upper) as seen by conn.
    // Note it will ignore those documents whose shardKey value type does not match the type of
    // lower/upper.
    function getNumDocs(conn, collName, lower, upper) {
        let coll = conn.getCollection(collName);
        let query = {$and: [{_id: {$gte: lower}}, {_id: {$lt: upper}}]};
        return itcount(coll, query);
    }

    // Intended for use on config or mongos connections only.
    // Get number of chunks containing values in [lower, upper). The upper bound on a chunk is
    // exclusive, but to capture the chunk we must provide it with less than or equal to 'upper'.
    function getNumChunks(conn, ns, lower, upper) {
        assert(
            isMongos(conn.getDB("admin")) || isMongodConfigsvr(conn.getDB("admin")),
            tojson(conn) + " is not to a mongos or a mongod config server",
        );
        assert(
            isString(ns) && ns.indexOf(".") !== -1 && !ns.startsWith(".") && !ns.endsWith("."),
            ns + " is not a valid namespace",
        );
        return findChunksUtil
            .findChunksByNs(conn.getDB("config"), ns, {"min._id": {$gte: lower}, "max._id": {$lte: upper}})
            .itcount();
    }

    // Intended for use on config or mongos connections only.
    // For getting chunks containing values in [lower, upper). The upper bound on a chunk is
    // exclusive, but to capture the chunk we must provide it with less than or equal to 'upper'.
    function getChunks(conn, ns, lower, upper) {
        assert(
            isMongos(conn.getDB("admin")) || isMongodConfigsvr(conn.getDB("admin")),
            tojson(conn) + " is not to a mongos or a mongod config server",
        );
        assert(
            isString(ns) && ns.indexOf(".") !== -1 && !ns.startsWith(".") && !ns.endsWith("."),
            ns + " is not a valid namespace",
        );
        return findChunksUtil
            .findChunksByNs(conn.getDB("config"), ns, {"min._id": {$gte: lower}, "max._id": {$lte: upper}})
            .sort({"min._id": 1})
            .toArray();
    }

    // Intended for use on config or mongos connections only.
    // For debug printing chunks containing values in [lower, upper). The upper bound on a chunk is
    // exclusive, but to capture the chunk we must provide it with less than or equal to 'upper'.
    function stringifyChunks(conn, lower, upper) {
        assert(
            isMongos(conn.getDB("admin")) || isMongodConfigsvr(conn.getDB("admin")),
            tojson(conn) + " is not to a mongos or a mongod config server",
        );
        return getChunks(conn, lower, upper)
            .map((chunk) => tojson(chunk))
            .join("\n");
    }

    return {
        splitChunkAt: splitChunkAt,
        splitChunkAtPoint: splitChunkAtPoint,
        splitChunkWithBounds: splitChunkWithBounds,
        moveChunk: moveChunk,
        mergeChunks: mergeChunks,
        getPrimary: getPrimary,
        getRandomMongos: getRandomMongos,
        getShardsForRange: getShardsForRange,
        getNumDocs: getNumDocs,
        getNumChunks: getNumChunks,
        getChunks: getChunks,
        stringifyChunks: stringifyChunks,
    };
})();
