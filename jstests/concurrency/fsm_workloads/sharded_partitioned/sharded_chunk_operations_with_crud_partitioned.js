/**
 * Extends sharded_base_partitioned.js.
 *
 * Runs all four chunk operations (moveChunk, splitChunk, mergeChunks, mergeAllChunksOnShard) and
 * CRUD operations on one sharded collection. Using per-thread tracked state, it checks that chunk
 * metadata changes never lose, duplicate, or stale documents, deadlock, or fail unexpectedly with a
 * non-retriable error.
 *
 * How it works:
 *   - Each thread owns its own `_id` range and only writes inside that range. So its in-memory
 *     `_id -> counter` map is the expected state for that range, and every read checks the
 *     collection against it.
 *   - Chunk ops are global: any thread can pick any chunk.
 *   - Each partition starts as one chunk with the low half populated. Chunk boundaries change
 *     over the run, but `_id` ownership stays fixed per thread, so the counts tracked in
 *     this.expectedDocumentCounters stay valid as chunks reshape.
 *   - Each chunk op outcome (committed vs. lost to a race) is counted by type and printed at the
 *     end. Teardown fails if nothing committed, so a run that did almost nothing cannot pass.
 *
 *              <== t0 partition ==>  <== t1 partition ==>  <== t2 partition ==>
 *    _id:     [ 0 ............ 199 )[ 200 .......... 399 )[ 400 .......... 599 )
 *    setup:   [###########........)[###########........)[###########........)
 *
 * @tags: [
 *  requires_sharding,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {fsm} from "jstests/concurrency/fsm_libs/fsm.js";
import {ChunkHelper} from "jstests/concurrency/fsm_workload_helpers/cluster_scalability/chunks.js";
import {ChunkOperationHelpers} from "jstests/concurrency/fsm_workload_helpers/cluster_scalability/chunk_operation_helpers.js";
import {
    ConcurrentOperation,
    Operation,
} from "jstests/concurrency/fsm_workload_helpers/cluster_scalability/chunk_operation_errors.js";
import {isErrorAcceptableWithConcurrent} from "jstests/concurrency/fsm_workload_helpers/cluster_scalability/chunk_operation_error_handler.js";
import {uniformDistTransitions} from "jstests/concurrency/fsm_workload_helpers/state_transition_utils.js";
import {findFirstBatch} from "jstests/concurrency/fsm_workload_helpers/stepdown_suite_helpers.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/sharded_partitioned/sharded_base_partitioned.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    $config.threadCount = 5;
    $config.iterations = 30;

    // 200 shard-key values per thread. The low half [lower, lower+100) is populated at setup.
    // The high half [lower+100, upper) is free for insert/remove.
    $config.data.partitionSize = 200;
    $config.data.populatedSize = 100;

    // --------------------------------------------------------------------------------------
    // Partition and chunk-selection helpers.
    // --------------------------------------------------------------------------------------

    $config.data.lowerHalf = function () {
        return {min: this.partition.lower, max: this.partition.lower + this.populatedSize};
    };

    $config.data.upperHalf = function () {
        return {min: this.partition.lower + this.populatedSize, max: this.partition.upper};
    };

    $config.data.randomIdIn = function (range) {
        return range.min + Random.randInt(range.max - range.min);
    };

    // Pick any chunk in the collection. Chunk ops are global, not per-partition: all threads
    // share the same pool of chunks.
    $config.data.getRandomChunk = function (ns, configConn) {
        const chunks = findChunksUtil.findChunksByNs(configConn.getDB("config"), ns).toArray();
        if (chunks.length === 0) {
            return null;
        }
        return chunks[Random.randInt(chunks.length)];
    };

    // Add the shared chunk-op helpers: race-error tolerance, the run-and-count wrapper, and the
    // success/failure counters.
    Object.assign($config.data, ChunkOperationHelpers);

    // Bucket labels for the teardown stats report.
    $config.data.trackedChunkOps = [
        Operation.MoveChunk,
        Operation.SplitChunk,
        Operation.MergeChunks,
        "mergeAllChunks",
    ];

    // --------------------------------------------------------------------------------------
    // Setup and init.
    //   - Setup populates the low half of each partition and splits each partition into its own
    //     chunk.
    //   - Init calls the base (which sets `this.partition`), then builds the per-thread
    //     `expectedDocumentCounters` map used by the CRUD states.
    // --------------------------------------------------------------------------------------

    $config.setup = function setup(db, collName, cluster) {
        const ns = db[collName].getFullName();
        const configDB = db.getSiblingDB("config");

        assert.gte(
            findChunksUtil.findChunksByNs(configDB, ns).itcount(),
            1,
            collName + " must be sharded",
        );

        for (let tid = 0; tid < this.threadCount; ++tid) {
            const partition = this.makePartition(ns, tid, this.partitionSize);

            const bulk = db[collName].initializeUnorderedBulkOp();
            for (let i = partition.lower; i < partition.lower + this.populatedSize; ++i) {
                bulk.insert({_id: i, counter: 0});
            }
            assert.commandWorked(bulk.execute({w: "majority"}));

            if (!partition.isLowChunk) {
                // With the balancer on, a concurrent migration can move the chunk between the
                // router's targeting and the shard's execution, so the split fails with a benign
                // metadata-churn race. Retry the whole split here until the boundary is set.
                assert.soon(() => {
                    try {
                        assert.commandWorked(
                            ChunkHelper.splitChunkAtPoint(db, collName, partition.lower),
                        );
                        return true;
                    } catch (e) {
                        // The same race set the workload body tolerates: StaleConfig, a BadValue
                        // split-precondition failure, and the per-shard contention codes.
                        if (
                            isErrorAcceptableWithConcurrent(
                                Operation.SplitChunk,
                                [
                                    ConcurrentOperation.MoveChunk,
                                    ConcurrentOperation.SplitChunk,
                                    ConcurrentOperation.MergeChunks,
                                ],
                                e,
                            )
                        ) {
                            return false;
                        }
                        throw e;
                    }
                }, `split at partition boundary ${partition.lower} did not succeed`);
            }
        }

        this.initChunkOpStats(db, collName);
    };

    $config.teardown = function teardown(db, collName, cluster) {
        this.dumpChunkOpStats(db, collName);
        $super.teardown.call(this, db, collName, cluster);
    };

    $config.states.init = function initState(db, collName, connCache) {
        $super.states.init.call(this, db, collName, connCache, true /* skipNumChunksCheck */);

        // _id -> expected counter, for every document this thread believes exists in its
        // partition. The pre-populated low half starts at counter 0.
        this.expectedDocumentCounters = {};
        for (let i = this.partition.lower; i < this.partition.lower + this.populatedSize; ++i) {
            this.expectedDocumentCounters[i] = 0;
        }

        jsTestLog(
            `tid=${this.tid} init: partition=[${this.partition.lower}, ${this.partition.upper})` +
                ` populated=[${this.partition.lower}, ${this.partition.lower + this.populatedSize})`,
        );
    };

    // --------------------------------------------------------------------------------------
    // Chunk operations.
    //
    // Every chunk-op state calls `fsm.forceRunningOutsideTransaction(this)`. Chunk commands take
    // config-server locks that can deadlock with an open transaction on the same session, so they
    // must run outside any transaction when this workload is used in transaction suites.
    // --------------------------------------------------------------------------------------

    $config.states.moveChunk = function moveChunkState(db, collName, connCache) {
        fsm.forceRunningOutsideTransaction(this);

        const ns = db[collName].getFullName();
        const config = connCache.rsConns.config;
        const numShards = findFirstBatch(config.getDB("config"), "shards", {}, 100).length;
        assert.gte(numShards, 2, "moveChunk requires at least 2 shards");

        const chunk = this.getRandomChunk(ns, config);
        assert(chunk, `tid ${this.tid}: no chunk found in namespace for moveChunk`);

        // No per-thread verify: CRUD runs on this chunk concurrently, so its state after the move
        // does not depend on this thread alone. Document correctness is checked by verifyPartition.
        this.runMoveChunk(db, collName, config, chunk);
    };

    $config.states.splitChunk = function splitChunkState(db, collName, connCache) {
        fsm.forceRunningOutsideTransaction(this);

        const ns = db[collName].getFullName();
        const config = connCache.rsConns.config;
        const chunk = this.getRandomChunk(ns, config);
        assert(chunk, `tid ${this.tid}: no chunk found in namespace for splitChunk`);

        this.runSplitChunk(db, collName, chunk);
    };

    $config.states.mergeChunks = function mergeChunksState(db, collName, connCache) {
        fsm.forceRunningOutsideTransaction(this);

        const ns = db[collName].getFullName();
        const config = connCache.rsConns.config;
        // Look at adjacent pairs across the whole namespace, not just the low end, so merges are
        // spread over the full keyspace.
        const chunks = findChunksUtil
            .findChunksByNs(config.getDB("config"), ns)
            .sort({"min._id": 1})
            .toArray();
        const pairs = this.findMergeableAdjacentPairs(chunks);
        if (pairs.length === 0) {
            return;
        }
        const pair = pairs[Random.randInt(pairs.length)];
        this.runMergeChunks(db, collName, pair);
    };

    // Unlike the other chunk ops, mergeAllChunksOnShard reports races as an ok:0 response instead
    // of throwing. So this state does not use runChunkOpTolerantOfConcurrentRaces; it checks the
    // tolerated codes directly with assert.commandWorkedOrFailedWithCode.
    $config.states.mergeAllChunks = function mergeAllChunksState(db, collName, connCache) {
        fsm.forceRunningOutsideTransaction(this);

        const ns = db[collName].getFullName();
        const config = connCache.rsConns.config;
        const shards = findFirstBatch(config.getDB("config"), "shards", {}, 100);
        const shard = shards[Random.randInt(shards.length)];

        jsTest.log.info("Running mergeAllChunksOnShard", {tid: this.tid, shard: shard._id});
        const res = db.adminCommand({mergeAllChunksOnShard: ns, shard: shard._id});
        this.recordChunkOpResult(db, collName, "mergeAllChunks", !!res.ok, res.ok ? null : res);
        if (!res.ok) {
            jsTest.log.info("mergeAllChunksOnShard failed", {
                tid: this.tid,
                shard: shard._id,
                res,
            });
        }
        // ConflictingOperationInProgress / LockBusy: another chunk op holds the namespace.
        // NamespaceNotFound: a shard add/remove or config change moved the collection off the
        // chosen shard before the command reached it, so it is no longer in that shard's local
        // catalog. All three are benign metadata-churn races.
        assert.commandWorkedOrFailedWithCode(res, [
            ErrorCodes.ConflictingOperationInProgress,
            ErrorCodes.LockBusy,
            ErrorCodes.NamespaceNotFound,
        ]);
    };

    // --------------------------------------------------------------------------------------
    // CRUD and verification.
    //
    // Each state runs one operation on this thread's partition and updates
    // `this.expectedDocumentCounters` to match the result.
    // --------------------------------------------------------------------------------------

    $config.states.insert = function insertState(db, collName, connCache) {
        const id = this.randomIdIn(this.upperHalf());
        const res = db[collName].insert({_id: id, counter: 0});

        if (res.nInserted === 1) {
            // A successful insert of `_id=id` must hit an untracked id. If `id` is already
            // tracked, the collection lost a document or the test's tracking drifted from it.
            assert(
                !(id in this.expectedDocumentCounters),
                `nInserted=1 for already-tracked _id ${id}`,
            );
            this.expectedDocumentCounters[id] = 0;
            return;
        }

        // The only allowed failure is a duplicate on an `_id` this thread inserted and has not
        // removed. mongos refreshes routing and retries inserts on StaleConfig before the shard
        // applies the write, so a "succeeded then reported duplicate" case is not expected here.
        assert(res.hasWriteError(), tojson(res));
        assert.eq(res.getWriteError().code, ErrorCodes.DuplicateKey, tojson(res));
        assert(
            id in this.expectedDocumentCounters,
            `DuplicateKey on _id ${id} but it was not tracked as live`,
        );
    };

    $config.states.update = function updateState(db, collName, connCache) {
        // Low-half ids are populated at setup and only this thread writes its partition, so the
        // update always matches exactly one document.
        const id = this.randomIdIn(this.lowerHalf());
        const res = db[collName].update({_id: id}, {$inc: {counter: 1}});

        assert.commandWorked(res);
        assert.eq(res.nMatched, 1, tojson(res));
        assert.eq(res.nModified, 1, tojson(res));
        this.expectedDocumentCounters[id] += 1;
    };

    $config.states.remove = function removeState(db, collName, connCache) {
        const id = this.randomIdIn(this.upperHalf());
        // `justOne` does a single-document delete (limit 1). A multi-delete (limit 0) is a
        // non-retryable write that stepdown suites refuse when the test asserts on the result.
        // Each `_id` is unique, so a single-document delete is equivalent here.
        const res = db[collName].remove({_id: id}, {justOne: true});
        assert.commandWorked(res);

        const docExisted = id in this.expectedDocumentCounters;
        if (res.nRemoved === 1) {
            assert(docExisted, `remove reported nRemoved: 1 for untracked _id ${id}`);
            delete this.expectedDocumentCounters[id];
        } else {
            assert.eq(res.nRemoved, 0, tojson(res));
            assert(!docExisted, `tracked _id ${id} survived remove: ${tojson(res)}`);
        }
    };

    $config.states.findOne = function findOneState(db, collName, connCache) {
        // Point-read a random tracked `_id` and check its counter. findOne gives a single shard
        // key, so mongos routes it to one shard — a different path from the range scan in
        // verifyPartition.
        const trackedIds = Object.keys(this.expectedDocumentCounters);
        assert.gt(trackedIds.length, 0, `tid ${this.tid}: expectedDocumentCounters is empty`);
        // Object keys are strings, but documents store `{_id: <number>}`, so convert back to a
        // number for the query.
        const id = Number(trackedIds[Random.randInt(trackedIds.length)]);
        const doc = db[collName].findOne({_id: id});
        assert(doc !== null, `expected _id ${id} not found by findOne`);
        assert.eq(
            doc.counter,
            this.expectedDocumentCounters[id],
            `counter mismatch for _id ${id}: expected ${this.expectedDocumentCounters[id]}, got ${doc.counter}`,
        );
    };

    $config.states.verifyPartition = function verifyPartitionState(db, collName, connCache) {
        const docs = findFirstBatch(
            db,
            collName,
            {_id: {$gte: this.partition.lower, $lt: this.partition.upper}},
            1e6,
        );

        const seen = {};
        for (const doc of docs) {
            assert(!(doc._id in seen), `duplicate _id ${doc._id} in find result`);
            seen[doc._id] = doc.counter;
        }

        const expected = this.expectedDocumentCounters;
        assert.eq(
            docs.length,
            Object.keys(expected).length,
            `partition doc count mismatch. expected=${tojson(expected)}, seen=${tojson(seen)}`,
        );
        for (const id in expected) {
            assert.eq(
                seen[id],
                expected[id],
                `counter mismatch for _id ${id}: expected ${expected[id]}, got ${seen[id]}`,
            );
        }
    };

    $config.transitions = uniformDistTransitions($config.states);
    return $config;
});
