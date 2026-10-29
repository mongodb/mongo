/**
 * Shared helpers for the partitioned chunk-operation FSM workloads.
 */
import {ChunkHelper} from "jstests/concurrency/fsm_workload_helpers/cluster_scalability/chunks.js";
import {
    ConcurrentOperation,
    Operation,
} from "jstests/concurrency/fsm_workload_helpers/cluster_scalability/chunk_operation_errors.js";
import {isErrorAcceptableWithConcurrent} from "jstests/concurrency/fsm_workload_helpers/cluster_scalability/chunk_operation_error_handler.js";
import {ShardingTopologyHelpers} from "jstests/concurrency/fsm_workload_helpers/catalog_and_routing/sharding_topology_helpers.js";

// Extracts the error message string from an error or error-like object.
function getMessage(e) {
    return e.message || e.errmsg || "";
}

// These helpers are installed onto a workload's `$config.data`. The FSM framework ships
// `$config.data` to each worker thread by BSON-serializing it: every function is encoded as a BSON
// Code value and re-evaluated in the worker as a standalone function expression. Method-shorthand
// (`name(args) {}`) is only valid inside an object literal, so its `.toString()` is rejected when
// re-evaluated alone ("SyntaxError: missing ) in parenthetical"). Each method is therefore written
// as an explicit `name: function (args) {}` property, which round-trips correctly.
export const ChunkOperationHelpers = {
    // Run chunk operation `fn` for `opName`, swallowing only the concurrent-op races acceptable for
    // that op. Returns {applied, error, message}: on commit, applied is true and error/message are
    // null; on a swallowed race, applied is false and error/message carry the error and its message.
    runChunkOpTolerantOfConcurrentRaces: function (opName, opDesc, fn) {
        const peerOps = [
            ConcurrentOperation.MoveChunk,
            ConcurrentOperation.SplitChunk,
            ConcurrentOperation.MergeChunks,
        ];
        try {
            assert.commandWorked(fn(), opDesc);
        } catch (e) {
            if (!isErrorAcceptableWithConcurrent(opName, peerOps, e)) {
                throw e;
            }
            jsTest.log.info("Chunk operation failed with concurrent-op race", {
                tid: this.tid,
                opDesc,
                error: e,
            });
            return {applied: false, error: e, message: getMessage(e)};
        }
        return {applied: true, error: null, message: null};
    },

    // --------------------------------------------------------------------------------------
    // Chunk-operation runners.
    //
    // Each runner takes the chunk (or pair) the caller picked and does the same tail: build the
    // description, log, run tolerant of concurrent-op races, and track the outcome. The two
    // workloads differ only in how they pick the chunk and whether they verify the result, so
    // callers pass an optional `verify` callback that runs only when the command committed.
    // --------------------------------------------------------------------------------------

    // Move `chunk` to a random other shard. Does nothing if there is only one shard.
    // `verify(ns, chunk, toShard, opDesc)` runs on commit.
    runMoveChunk: function (db, collName, config, chunk, verify) {
        const ns = db[collName].getFullName();
        const shardNames = ShardingTopologyHelpers.getShardNames(db);
        const destinations = shardNames.filter((s) => s !== chunk.shard);
        if (destinations.length === 0) {
            return;
        }

        const toShard = destinations[Random.randInt(destinations.length)];
        const bounds = [{_id: chunk.min._id}, {_id: chunk.max._id}];
        const waitForDelete = Random.rand() < 0.5;

        const opDesc =
            `moveChunk bounds=[${tojsononeline(chunk.min._id)}, ${tojsononeline(chunk.max._id)})` +
            ` from=${chunk.shard} to=${toShard} waitForDelete=${waitForDelete}`;
        jsTest.log.info("Running moveChunk", {tid: this.tid, opDesc});

        const {applied, error} = this.runChunkOpTolerantOfConcurrentRaces(
            Operation.MoveChunk,
            opDesc,
            () => ChunkHelper.moveChunk(db, collName, bounds, toShard, waitForDelete),
        );

        this.recordChunkOpResult(db, collName, Operation.MoveChunk, applied, error);
        if (applied && verify) {
            verify(ns, chunk, toShard, opDesc);
        }
    },

    // Split `chunk`, letting `split` pick the point. Does nothing for a chunk with fewer than two
    // numeric shard-key values (a MinKey/MaxKey bound is unbounded, so always splittable).
    // `verify(ns, lo, hi, shard, opDesc)` runs on commit.
    runSplitChunk: function (db, collName, chunk, verify) {
        const ns = db[collName].getFullName();
        const lo = chunk.min._id;
        const hi = chunk.max._id;
        if (typeof lo === "number" && typeof hi === "number" && hi - lo < 2) {
            return;
        }

        const opDesc = `splitChunk bounds=[${tojsononeline(lo)}, ${tojsononeline(hi)}) on=${chunk.shard}`;
        jsTest.log.info("Running splitChunk", {tid: this.tid, opDesc});

        const {applied, error} = this.runChunkOpTolerantOfConcurrentRaces(
            Operation.SplitChunk,
            opDesc,
            () => ChunkHelper.splitChunkWithBounds(db, collName, [{_id: lo}, {_id: hi}]),
        );

        this.recordChunkOpResult(db, collName, Operation.SplitChunk, applied, error);
        if (applied && verify) {
            verify(ns, lo, hi, chunk.shard, opDesc);
        }
    },

    // Merge the adjacent co-located pair `[a, b]` into one chunk.
    // `verify(ns, aMin, bMax, shard, opDesc)` runs on commit.
    runMergeChunks: function (db, collName, pair, verify) {
        const ns = db[collName].getFullName();
        const [a, b] = pair;

        const opDesc =
            `mergeChunks bounds=[${tojsononeline(a.min._id)}, ${tojsononeline(b.max._id)})` +
            ` on=${a.shard}`;
        jsTest.log.info("Running mergeChunks", {tid: this.tid, opDesc});

        const {applied, error} = this.runChunkOpTolerantOfConcurrentRaces(
            Operation.MergeChunks,
            opDesc,
            () => ChunkHelper.mergeChunks(db, collName, [a.min, b.max]),
        );

        this.recordChunkOpResult(db, collName, Operation.MergeChunks, applied, error);
        if (applied && verify) {
            verify(ns, a.min, b.max, a.shard, opDesc);
        }
    },

    // Given `chunks` sorted by min._id, return every [a, b] pair of adjacent chunks on the same
    // shard. These are the pairs mergeChunks can merge, so the caller can pick one at random.
    findMergeableAdjacentPairs: function (chunks) {
        const pairs = [];
        for (let i = 0; i < chunks.length - 1; ++i) {
            const a = chunks[i];
            const b = chunks[i + 1];
            if (a.shard === b.shard && bsonWoCompare(a.max, b.min) === 0) {
                pairs.push([a, b]);
            }
        }
        return pairs;
    },

    // --------------------------------------------------------------------------------------
    // Per-operation success/failure counters.
    //
    // The functions below track how each chunk op turned out, so teardown can report what the run
    // exercised and fail if nothing committed. The counts live in a stats collection in the
    // database: `initChunkOpStats` pre-creates one document per op, `recordChunkOpResult` bumps the
    // counters after each op, and `dumpChunkOpStats` reads them at the end.
    // --------------------------------------------------------------------------------------

    // Name of the per-collection stats collection.
    statsCollName: function (collName) {
        return collName + "_chunk_op_stats";
    },

    // Pre-create one stats document per tracked op so threads only $inc, never upsert.
    initChunkOpStats: function (db, collName) {
        const stats = db[this.statsCollName(collName)];
        for (const op of this.trackedChunkOps) {
            assert.commandWorked(
                stats.update(
                    {_id: op},
                    // `errors` holds a label -> count map of the swallowed concurrent-op races.
                    {$setOnInsert: {succeeded: 0, raceFailed: 0, errors: {}}},
                    {upsert: true},
                ),
            );
        }
    },

    // Record the outcome of one chunk op in the stats document for `opName`. If it committed, bump
    // `succeeded`; otherwise bump `raceFailed` and, when `error` is set, bump its code-name bucket
    // count under `errors` so the teardown report shows which races the op hit.
    recordChunkOpResult: function (db, collName, opName, succeeded, error) {
        const inc = {[succeeded ? "succeeded" : "raceFailed"]: 1};
        if (!succeeded && error) {
            const chunkOpErrorLabel = error.codeName || "code_" + error.code;
            inc["errors." + chunkOpErrorLabel] = 1;
        }
        assert.commandWorked(db[this.statsCollName(collName)].update({_id: opName}, {$inc: inc}));
    },

    // Log a per-operation summary and fail if nothing committed over the whole run. Called from
    // teardown, before any framework cleanup, so the counts are intact.
    dumpChunkOpStats: function (db, collName) {
        const rows = db[this.statsCollName(collName)].find().sort({_id: 1}).toArray();
        const summary = {};
        let totalSucceeded = 0;
        for (const r of rows) {
            const attempted = r.succeeded + r.raceFailed;
            totalSucceeded += r.succeeded;
            // Order the swallowed-race buckets by descending frequency for readability.
            const errors = {};
            for (const [key, count] of Object.entries(r.errors || {}).sort((a, b) => b[1] - a[1])) {
                errors[key] = count;
            }
            summary[r._id] = {
                succeeded: r.succeeded,
                raceFailed: r.raceFailed,
                attempted,
                successRatePct:
                    attempted === 0 ? null : Math.round((100 * r.succeeded) / attempted),
                errors,
            };
        }
        jsTest.log.info("Chunk operation outcomes", {summary});
        assert.gt(
            totalSucceeded,
            0,
            "no chunk operation committed over the whole run; the test exercised nothing",
        );
        db[this.statsCollName(collName)].drop();
    },
};
