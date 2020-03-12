'use strict';

/**
 * Provides wrapper functions that perform exponential backoff and allow for
 * acceptable errors to be returned from mergeChunks, moveChunk, and splitChunk
 * commands.
 *
 * Also provides functions to help perform assertions about the state of chunks.
 *
 * Intended for use by workloads testing sharding (i.e., workloads starting with 'sharded_').
 */

load('jstests/concurrency/fsm_workload_helpers/server_types.js');  // for isMongos & isMongod

var ChunkHelper = (function() {
    // exponential backoff
    function getNextBackoffSleep(curSleep) {
        const MAX_BACKOFF_SLEEP = 5000;  // milliseconds

        curSleep *= 2;
        return Math.min(curSleep, MAX_BACKOFF_SLEEP);
    }

    function runCommandWithRetries(db, cmd, didAcceptableErrorOccurFn) {
        const INITIAL_BACKOFF_SLEEP = 500;  // milliseconds
        const MAX_RETRIES = 5;

        var res;
        var retries = 0;
        var backoffSleep = INITIAL_BACKOFF_SLEEP;
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
            assertWhenOwnColl.commandWorked(res, cmd);
        }

        return res;
    }

    function splitChunkAtPoint(db, collName, splitPoint) {
        var cmd = {split: db[collName].getFullName(), middle: {_id: splitPoint}};
        return runCommandWithRetries(db, cmd, res => res.code === ErrorCodes.LockBusy);
    }

    function splitChunkWithBounds(db, collName, bounds) {
        var cmd = {split: db[collName].getFullName(), bounds: bounds};
        return runCommandWithRetries(db, cmd, res => res.code === ErrorCodes.LockBusy);
    }

    function moveChunk(db, collName, bounds, toShard, waitForDelete) {
        var cmd = {
            moveChunk: db[collName].getFullName(),
            bounds: bounds,
            to: toShard,
            _waitForDelete: waitForDelete
        };

        const runningWithStepdowns =
            TestData.runningWithConfigStepdowns || TestData.runningWithShardStepdowns;

        return runCommandWithRetries(
            db,
            cmd,
            res => (res.code === ErrorCodes.ConflictingOperationInProgress ||
                    res.code === ErrorCodes.ChunkRangeCleanupPending ||
                    // The chunk migration has surely been aborted if the startCommit of the
                    // procedure was interrupted by a stepdown.
                    (runningWithStepdowns && res.code === ErrorCodes.CommandFailed &&
                     res.errmsg.includes("startCommit")) ||
                    // The chunk migration has surely been aborted if the recipient shard didn't
                    // believe there was an active chunk migration.
                    (runningWithStepdowns && res.code === ErrorCodes.OperationFailed &&
                     res.errmsg.includes("NotYetInitialized"))));
    }

    function mergeChunks(db, collName, bounds) {
        var cmd = {mergeChunks: db[collName].getFullName(), bounds: bounds};
        return runCommandWithRetries(db, cmd, res => res.code === ErrorCodes.LockBusy);
    }

    // Take a set of connections to a shard (replica set or standalone mongod),
    // or a set of connections to the config servers, and return a connection
    // to any node in the set for which ismaster is true.
    function getPrimary(connArr) {
        const kDefaultTimeoutMS = 10 * 60 * 1000;  // 10 minutes.
        assertAlways(Array.isArray(connArr), 'Expected an array but got ' + tojson(connArr));

        let primary = null;
        assert.soon(() => {
            for (let conn of connArr) {
                assert(isMongod(conn.getDB('admin')), tojson(conn) + ' is not to a mongod');
                let res = conn.adminCommand({isMaster: 1});
                assertAlways.commandWorked(res);

                if (res.ismaster) {
                    primary = conn;
                    return primary;
                }
            }
        }, 'Finding primary timed out', kDefaultTimeoutMS);

        return primary;
    }

    // Take a set of mongos connections to a sharded cluster and return a
    // random connection.
    function getRandomMongos(connArr) {
        assertAlways(Array.isArray(connArr), 'Expected an array but got ' + tojson(connArr));
        var conn = connArr[Random.randInt(connArr.length)];
        assert(isMongos(conn.getDB('admin')), tojson(conn) + ' is not to a mongos');
        return conn;
    }

    // Intended for use on mongos connections only.
    // Return all shards containing documents in [lower, upper).
    function getShardsForRange(conn, collName, lower, upper) {
        assert(isMongos(conn.getDB('admin')), tojson(conn) + ' is not to a mongos');
        var adminDB = conn.getDB('admin');
        var shardVersion = adminDB.runCommand({getShardVersion: collName, fullMetadata: true});
        assertAlways.commandWorked(shardVersion);
        // As noted in SERVER-20768, doing a range query with { $lt : X },  where
        // X is the _upper bound_ of a chunk,  incorrectly targets the shard whose
        // _lower bound_ is X. Therefore, if upper !== MaxKey, we use a workaround
        // to ensure that only the shard whose lower bound = X is targeted.
        var query;
        if (upper === MaxKey) {
            query = {$and: [{_id: {$gte: lower}}, {_id: {$lt: upper}}]};
        } else {
            query = {$and: [{_id: {$gte: lower}}, {_id: {$lte: upper - 1}}]};
        }
        var res = conn.getCollection(collName).find(query).explain();
        assertAlways.commandWorked(res);
        assertAlways.gt(
            res.queryPlanner.winningPlan.shards.length, 0, 'Explain did not have shards key.');

        var shards = res.queryPlanner.winningPlan.shards.map(shard => shard.shardName);
        return {shards: shards, explain: res, query: query, shardVersion: shardVersion};
    }

    function itcount(collection, query) {
        // We project out all of the fields in order to greatly reduce the likelihood a cursor would
        // actually be returned. This is acceptable because we're only interested in how many
        // documents there were and not any of their contents. The network_error_and_txn_override.js
        // override would throw an exception if we attempted to use the getMore command.
        return collection.find(query, {_id: 0, nonExistingField: 1}).itcount();
    }

    // Return the number of docs in [lower, upper) as seen by conn.
    function getNumDocs(conn, collName, lower, upper) {
        var coll = conn.getCollection(collName);
        var query = {$and: [{_id: {$gte: lower}}, {_id: {$lt: upper}}]};
        return itcount(coll, query);
    }

    // Intended for use on config or mongos connections only.
    // Get number of chunks containing values in [lower, upper). The upper bound on a chunk is
    // exclusive, but to capture the chunk we must provide it with less than or equal to 'upper'.
    function getNumChunks(conn, ns, lower, upper) {
        assert(isMongos(conn.getDB('admin')) || isMongodConfigsvr(conn.getDB('admin')),
               tojson(conn) + ' is not to a mongos or a mongod config server');
        assert(isString(ns) && ns.indexOf('.') !== -1 && !ns.startsWith('.') && !ns.endsWith('.'),
               ns + ' is not a valid namespace');
        var query = {'ns': ns, 'min._id': {$gte: lower}, 'max._id': {$lte: upper}};
        return itcount(conn.getDB('config').chunks, query);
    }

    // Intended for use on config or mongos connections only.
    // For getting chunks containing values in [lower, upper). The upper bound on a chunk is
    // exclusive, but to capture the chunk we must provide it with less than or equal to 'upper'.
    function getChunks(conn, ns, lower, upper) {
        assert(isMongos(conn.getDB('admin')) || isMongodConfigsvr(conn.getDB('admin')),
               tojson(conn) + ' is not to a mongos or a mongod config server');
        assert(isString(ns) && ns.indexOf('.') !== -1 && !ns.startsWith('.') && !ns.endsWith('.'),
               ns + ' is not a valid namespace');
        var query = {'ns': ns, 'min._id': {$gte: lower}, 'max._id': {$lte: upper}};
        return conn.getDB('config').chunks.find(query).sort({'min._id': 1}).toArray();
    }

    // Intended for use on config or mongos connections only.
    // For debug printing chunks containing values in [lower, upper). The upper bound on a chunk is
    // exclusive, but to capture the chunk we must provide it with less than or equal to 'upper'.
    function stringifyChunks(conn, lower, upper) {
        assert(isMongos(conn.getDB('admin')) || isMongodConfigsvr(conn.getDB('admin')),
               tojson(conn) + ' is not to a mongos or a mongod config server');
        return getChunks(conn, lower, upper).map(chunk => tojson(chunk)).join('\n');
    }

    return {
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
        stringifyChunks: stringifyChunks
    };
})();
