/**
 * SERVER-115121 repro: keysExamined / docsExamined render as negative values in
 * mongod slow-query logs when the merging shard / mongos accumulates per-shard
 * CursorMetrics via OpDebug::AdditiveMetrics::aggregateCursorMetrics().
 *
 * Root cause (see SERVER-115121-DESIGN.md sibling file in this branch):
 *
 *   src/mongo/db/op_debug.cpp:1741-1742
 *       static_cast<uint64_t>(metrics.getKeysExamined())
 *
 *   The CursorMetrics IDL field is `long` (signed int64). aggregateCursorMetrics
 *   casts to `uint64_t`, hands it to DataBearingNodeMetrics (which stores
 *   uint64_t fields), then op_debug.cpp:1662 does
 *
 *       keysExamined = keysExamined.value_or(0) + metrics.keysExamined;
 *
 *   where the LHS is boost::optional<long long> and the RHS is uint64_t. The
 *   addition is performed in uint64_t arithmetic and silently reassigned to a
 *   long long, so any cumulative sum >= 2^63 reappears with the sign bit set.
 *   The raw mongod log encodes the BSON int64 correctly, but `lq`/`lv` parsers
 *   render the high-bit-set value as a NumberLong with a leading minus -- which
 *   is precisely the symptom in the Slack thread linked from the ticket
 *   description (lv/lq vs text-editor disparity).
 *
 *   The same cast pattern also corrupts the value immediately if any shard ever
 *   reports a transient negative `keysExamined` (e.g. an un-set IDL field that
 *   was decoded from a malformed/empty subobject as -1): static_cast<uint64_t>
 *   of a negative signed long produces ~2^64, which then gets summed back into
 *   the merging accumulator and flipped to a large negative long long on the
 *   final assignment.
 *
 * This test stands up a 2-shard sharded cluster, configures profiling so the
 * mongos's accumulated metrics are written to its slow-query log, runs a
 * fan-out aggregation that exercises the merge-cursor / getMore path
 * (DocumentSourceMergeCursors -> aggregateCursorMetrics) repeatedly, and then
 * parses the resulting slow-query log line. The invariant checked is the only
 * one the customer cares about: keysExamined and docsExamined are non-negative
 * integers in the rendered log.
 *
 * Pre-fix: test FAILS once the merging accumulator either (a) observes a
 *   negative per-shard CursorMetrics field (the un-set-field path), or
 *   (b) crosses 2^63 cumulatively across many getMore rounds.
 * Post-fix: test PASSES because the accumulator never narrows a uint64_t into
 *   a signed long long, and out-of-range shard inputs are rejected explicitly.
 *
 * NOTE for the eventual fixer: the documented behavioral repro below uses a
 * realistic-but-modest workload. The 2^63 wrap is not directly synthesizable
 * with real documents -- to exercise that branch in CI you will want to add
 * a `MONGO_FAIL_POINT_DEFINE(injectCursorMetricsForTesting)` in
 * src/mongo/db/op_debug.cpp::OpDebug::getCursorMetrics() that lets the test
 * stuff a chosen long value into the outbound CursorMetrics on the shard
 * side; the matching test stanza is sketched at the bottom of this file.
 *
 * @tags: [
 *   requires_sharding,
 *   requires_profiling,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findMatchingLogLine} from "jstests/libs/log.js";

const st = new ShardingTest({mongos: 1, shards: 2, rs: {nodes: 1}});

const mongos = st.s0;
const dbName = "server115121";
const collName = "kx_dx_negative_repro";
const db = mongos.getDB(dbName);
const coll = db.getCollection(collName);

// Shard the collection so the aggregation actually fans out and the mongos
// must accumulate per-shard CursorMetrics via aggregateCursorMetrics().
assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));
assert.commandWorked(
    mongos.adminCommand({shardCollection: `${dbName}.${collName}`, key: {_id: "hashed"}}));

// Enough docs to force multiple getMore rounds per shard at the default batch
// size; each getMore round re-enters aggregateCursorMetrics on the mongos.
const N = 4000;
const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < N; ++i) {
    bulk.insert({_id: i, a: i % 17, payload: "x".repeat(64)});
}
assert.commandWorked(bulk.execute());

// Force the mongos to log every command so the merge-cursor path's accumulated
// metrics land in the slow-query log we will parse below.
assert.commandWorked(db.adminCommand({profile: 0, slowms: -1}));
for (const shard of [st.rs0.getPrimary(), st.rs1.getPrimary()]) {
    assert.commandWorked(shard.getDB(dbName).runCommand({profile: 0, slowms: -1}));
}

const comment = "server115121-keys-examined-negative-repro";

// Aggregation crafted to look at every shard, examine many index keys and
// documents per shard, and produce multiple getMore rounds at the merge cursor.
// $group with $sum forces full consumption; the {a: {$gte: 0}} predicate
// guarantees a non-empty plan summary so per-shard CursorMetrics.keysExamined
// and CursorMetrics.docsExamined are populated.
const pipeline = [
    {$match: {a: {$gte: 0}}},
    {$group: {_id: "$a", n: {$sum: 1}, blob: {$push: "$payload"}}},
    {$sort: {_id: 1}},
];

const cursor = coll.aggregate(pipeline, {comment: comment, allowDiskUse: true, batchSize: 32});
// Drain so all getMore rounds fire and the mongos finalizes its OpDebug.
assert.gt(cursor.itcount(), 0);

const globalLog = assert.commandWorked(db.adminCommand({getLog: "global"}));
const line = findMatchingLogLine(globalLog.log, {msg: "Slow query", comment: comment});
assert(line, `Slow query log line not found for comment ${comment}`);

jsTestLog("Mongos slow-query log line for SERVER-115121 repro:\n" + line);

// Parse keysExamined / docsExamined out of the log line. The exact JSON
// rendering of NumberLong differs across log formats; cover both the
// JSON-line form (`"keysExamined":N`) and the legacy form (`keysExamined:N`),
// allowing an optional leading minus -- that minus is the symptom we are
// here to fail on.
function parseMetric(name) {
    const re = new RegExp(`["']?${name}["']?\\s*:\\s*(-?\\d+)`);
    const m = line.match(re);
    assert(m, `Could not locate ${name} in slow-query log line:\n${line}`);
    return parseInt(m[1], 10);
}

const keysExamined = parseMetric("keysExamined");
const docsExamined = parseMetric("docsExamined");

jsTestLog(`Parsed keysExamined=${keysExamined}, docsExamined=${docsExamined}`);

// THE INVARIANT. Both must be non-negative on the merging side.
assert.gte(keysExamined,
           0,
           `SERVER-115121: keysExamined is negative (${keysExamined}) in slow-query log -- ` +
               `uint64_t -> long long narrowing in OpDebug::AdditiveMetrics::aggregateCursorMetrics. ` +
               `Full log line:\n${line}`);
assert.gte(docsExamined,
           0,
           `SERVER-115121: docsExamined is negative (${docsExamined}) in slow-query log -- ` +
               `uint64_t -> long long narrowing in OpDebug::AdditiveMetrics::aggregateCursorMetrics. ` +
               `Full log line:\n${line}`);

// Sanity: with N docs across 2 shards, every doc was examined at least once.
assert.gte(docsExamined,
           N,
           `docsExamined (${docsExamined}) below floor N=${N}; the aggregation ` +
               `did not actually fan out -- adjust the pipeline before declaring this passes.`);

st.stop();

/*
 * APPENDIX -- direct overflow stanza (depends on adding a failpoint in
 *             OpDebug::getCursorMetrics on the shard side; see SERVER-115121-
 *             DESIGN.md "Test-only failpoint" section). Once that failpoint
 *             lands, the following block deterministically triggers the
 *             uint64 -> long long sign-bit flip with a single getMore round:
 *
 *   const NEAR_MAX = NumberLong("9223372036854775000");  // INT64_MAX - 807
 *   for (const shard of [st.rs0.getPrimary(), st.rs1.getPrimary()]) {
 *       assert.commandWorked(shard.adminCommand({
 *           configureFailPoint: "injectCursorMetricsForTesting",
 *           mode: "alwaysOn",
 *           data: {keysExamined: NEAR_MAX, docsExamined: NEAR_MAX},
 *       }));
 *   }
 *   // ... rerun the aggregate above. The two NEAR_MAX values, summed by
 *   // mongos via aggregateCursorMetrics, overflow on the second shard's
 *   // contribution and reappear as a large negative NumberLong in the log.
 */
