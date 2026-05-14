// Tests that $merge is idempotent under replay: running the same deterministic
// aggregation pipeline twice against the same unchanged source collection must
// produce the same target-collection state.
//
// The documented contract of $merge with whenMatched in {replace, merge,
// keepExisting} (each paired with whenNotMatched: "insert") is that a replay
// over unchanged input is a fixed point on observable target state. This
// invariant is load-bearing for materialized-view rematerialization and for
// exactly-once stream-processing patterns built on top of $merge, both of
// which rely on the second application of the same pipeline being a no-op on
// observable state.
//
// The test exercises all three whenMatched modes. For each mode, we run the
// same $group-then-$merge pipeline twice over an unchanged source, then
// assert:
//
//   (a) orderedArrayEq(target.find().sort({_id:1}).toArray()) after run 1 ==
//       orderedArrayEq(target.find().sort({_id:1}).toArray()) after run 2
//   (b) a deterministic fingerprint (sum of grouped values + doc count) is
//       equal across the two runs
//
// If a future server patch causes $merge replay to mutate target state on the
// second run (e.g. by losing whenMatched semantics, by misordering the upsert,
// or by introducing nondeterministic _id generation in a path that should be
// upsert-keyed), this test fails.
import {dropWithoutImplicitRecreate} from "jstests/aggregation/extras/merge_helpers.js";
import {orderedArrayEq} from "jstests/aggregation/extras/utils.js";

const source = db[`${jsTest.name()}_source`];
const target = db[`${jsTest.name()}_target`];

// Deterministic event stream. Each event carries a bucket id in [0, 9] so the
// group stage produces exactly 10 target documents, and a unit `value` so the
// grouped sum equals the per-bucket event count. With 1000 events evenly
// distributed across 10 buckets, every bucket receives exactly 100 events,
// every grouped value is 100, and the fingerprint (Σ value) is 1000.
const N_EVENTS = 1000;
const N_BUCKETS = 10;

function seedSource() {
    source.drop();
    const docs = [];
    for (let i = 0; i < N_EVENTS; i++) {
        docs.push({event_id: i, bucket: i % N_BUCKETS, value: 1, ts: i});
    }
    assert.commandWorked(source.insert(docs));
    assert.eq(N_EVENTS, source.find().itcount());
}

// The $merge pipeline factory. Materializes per-bucket counts and sums into
// the target collection. _id is the bucket id, so $merge's default "on" field
// is _id and we exercise the upsert path without needing a separate unique
// index. Output schema: {_id: <bucket>, count: <int>, sum: <int>}.
function buildPipeline(whenMatched) {
    return [
        {$group: {_id: "$bucket", count: {$sum: 1}, sum: {$sum: "$value"}}},
        {
            $merge: {
                into: target.getName(),
                whenMatched: whenMatched,
                whenNotMatched: "insert",
            },
        },
    ];
}

// Deterministic fingerprint over the target collection. We pin both the
// aggregate sum-of-sums (which equals N_EVENTS under any idempotent run) and
// the document count (which equals N_BUCKETS), so a replay that drifts by
// either a duplicate insert or a double-applied update is caught.
function fingerprint() {
    const docs = target.find().sort({_id: 1}).toArray();
    let sumOfSums = 0;
    let sumOfCounts = 0;
    for (const d of docs) {
        // `sum` and `count` are absent only if a future bug returns the
        // bare _id-keyed shell of a $merge document; treat that as 0 rather
        // than NaN so the fingerprint stays comparable.
        sumOfSums += (typeof d.sum === "number") ? d.sum : 0;
        sumOfCounts += (typeof d.count === "number") ? d.count : 0;
    }
    return {nDocs: docs.length, sumOfSums: sumOfSums, sumOfCounts: sumOfCounts};
}

// One sub-test per whenMatched mode. Each: seed source, drop target, run
// pipeline twice, assert both snapshots and fingerprints are equal.
function testReplayIdempotency(whenMatched) {
    seedSource();
    dropWithoutImplicitRecreate(target.getName());

    const pipeline = buildPipeline(whenMatched);

    // Round 1.
    assert.doesNotThrow(() => source.aggregate(pipeline));
    const snapshot1 = target.find().sort({_id: 1}).toArray();
    const fp1 = fingerprint();

    // Sanity check the baseline post-round-1 shape, so a mis-seeded source
    // doesn't masquerade as a successful idempotency test.
    assert.eq(N_BUCKETS, snapshot1.length,
              `expected ${N_BUCKETS} buckets after round 1 for ` +
              `whenMatched=${whenMatched}, got ${snapshot1.length}`);
    assert.eq(N_EVENTS, fp1.sumOfSums,
              `expected Σsum == ${N_EVENTS} after round 1 for ` +
              `whenMatched=${whenMatched}, got ${fp1.sumOfSums}`);
    assert.eq(N_EVENTS, fp1.sumOfCounts,
              `expected Σcount == ${N_EVENTS} after round 1 for ` +
              `whenMatched=${whenMatched}, got ${fp1.sumOfCounts}`);

    // Round 2 — same pipeline, same unchanged source. This is the replay leg.
    // Under $merge's documented contract this must be a no-op on observable
    // state for whenMatched=replace and whenMatched=keepExisting, and a fixed
    // point for whenMatched=merge (since the same merged document is folded
    // into itself, an idempotent fold).
    assert.doesNotThrow(() => source.aggregate(pipeline));
    const snapshot2 = target.find().sort({_id: 1}).toArray();
    const fp2 = fingerprint();

    // The core invariant. We use orderedArrayEq because we already sorted
    // both snapshots on _id, and we want field-by-field equality not just
    // set-equality (a regression that swaps `count` and `sum` would slip
    // past assertArrayEq's set semantics).
    assert(orderedArrayEq(snapshot1, snapshot2),
           `replay mutated target state for whenMatched=${whenMatched}.\n` +
           `  snapshot1: ${tojson(snapshot1)}\n` +
           `  snapshot2: ${tojson(snapshot2)}`);

    assert.eq(fp1.nDocs, fp2.nDocs,
              `replay changed doc count for whenMatched=${whenMatched}: ` +
              `${fp1.nDocs} -> ${fp2.nDocs}`);
    assert.eq(fp1.sumOfSums, fp2.sumOfSums,
              `replay changed Σsum for whenMatched=${whenMatched}: ` +
              `${fp1.sumOfSums} -> ${fp2.sumOfSums}`);
    assert.eq(fp1.sumOfCounts, fp2.sumOfCounts,
              `replay changed Σcount for whenMatched=${whenMatched}: ` +
              `${fp1.sumOfCounts} -> ${fp2.sumOfCounts}`);
}

// whenMatched: "replace" — the canonical idempotent mode. Round 2 overwrites
// each target doc with bit-identical content; the post-state must equal the
// pre-state.
(function testWhenMatchedReplaceIsReplayIdempotent() {
    testReplayIdempotency("replace");
})();

// whenMatched: "merge" — folds source fields into target. Replaying the same
// fold of the same source over the same target is a fixed point: each field
// is overwritten with the same value it already held.
(function testWhenMatchedMergeIsReplayIdempotent() {
    testReplayIdempotency("merge");
})();

// whenMatched: "keepExisting" — strictly stronger idempotency than "replace":
// the existing target doc is preserved untouched on every match. Replay must
// therefore be a literal no-op.
(function testWhenMatchedKeepExistingIsReplayIdempotent() {
    testReplayIdempotency("keepExisting");
})();
