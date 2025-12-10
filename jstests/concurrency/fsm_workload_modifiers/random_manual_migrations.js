/**
 * This file adds stages to perform random moveChunk/moveRange commands. It is designed to be
 * compatible with the balancer being enabled and will ignore any errors that come from concurrent
 * chunk splits/moves.
 *
 * To specify the collection that migrations should be run on, a test can define the
 * `$config.data.collWithMigrations` which will be used by the moveChunk stage in this file.
 */
import {fsm} from "jstests/concurrency/fsm_libs/fsm.js";
import {ChunkHelper} from "jstests/concurrency/fsm_workload_helpers/chunks.js";
import {isMoveChunkErrorAcceptableWithConcurrent} from "jstests/concurrency/fsm_workload_helpers/cluster_scalability/move_chunk_errors.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

export function randomManualMigration($config, $super) {
    $config.data.getConcurrentOperations = () => [];

    $config.states.moveChunk = function moveChunk(db, collName, connCache) {
        // Some tests want to specify a collection to run chunk migrations on whereas others want to
        // use the default. Check here if the collWithMigrations field is set and use it if so.
        let moveChunkCollName = this.collWithMigrations ? this.collWithMigrations : collName;

        // Committing a chunk migration requires acquiring the global X lock on the CSRS primary.
        // This state function is unsafe to automatically run inside a multi-statement transaction
        // because it'll have left an idle transaction on the CSRS primary before attempting to run
        // the moveChunk command, which can lead to a hang.
        fsm.forceRunningOutsideTransaction(this);
        const configDB = connCache.rsConns.config.getDB("config");

        // Get a chunk from config.chunks - this may be stale by the time we issue the migration but
        // this will be handled in the acceptable errors.
        const chunksJoinClause = findChunksUtil.getChunksJoinClause(configDB, db.getName() + "." + moveChunkCollName);
        let chunks = configDB
            .getCollection("chunks")
            .aggregate([{$match: chunksJoinClause}, {$sample: {size: 1}}])
            .toArray();
        // If there are no chunks, return early.
        if (chunks.length == 0) {
            return;
        }
        const chunk = chunks[0];
        const fromShard = chunk.shard;

        // Get a shard from config.shards rather than the connCache so that we can do a best-effort
        // filter of draining shards.
        let shards = configDB
            .getCollection("shards")
            .aggregate([{$match: {"_id": {$ne: fromShard}}}, {$match: {"draining": {$ne: true}}}, {$sample: {size: 1}}])
            .toArray();
        // If there are no non-draining shards, return early.
        if (shards.length == 0) {
            return;
        }
        const toShard = shards[0];

        // Use chunk_helper.js's moveChunk wrapper to tolerate acceptable failures and to use a
        // limited number of retries with exponential backoff.
        const waitForDelete = Random.rand() < 0.5;
        const secondaryThrottle = Random.rand() < 0.5;
        try {
            ChunkHelper.moveChunk(
                db,
                moveChunkCollName,
                [chunk.min, chunk.max],
                toShard._id,
                waitForDelete,
                secondaryThrottle,
            );
        } catch (e) {
            // Failed moveChunks are thrown by the moveChunk helper with the response included as a
            // JSON string in the error's message.
            if (isMoveChunkErrorAcceptableWithConcurrent(this.getConcurrentOperations(), e)) {
                print("Ignoring acceptable moveChunk error: " + tojson(e));
                return;
            }

            throw e;
        }
    };

    return $config;
}
