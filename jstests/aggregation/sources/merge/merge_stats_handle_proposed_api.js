/**
 * Pins the current (no-stats) behavior of $merge and $out, and documents the proposed
 * opt-in `{getMergeStats: true}` cursor option that closes SERVER-43194.
 *
 * Related ticket: SERVER-43194 — "provide a way to get result/outcome of $merge or $out".
 * Design doc:    src/mongo/db/pipeline/MERGE_STATS_DESIGN.md
 *
 * --------------------------------------------------------------------------------
 * Proposed API (NOT YET IMPLEMENTED — this test file documents the shape):
 * --------------------------------------------------------------------------------
 *
 *   db.runCommand({
 *       aggregate: "source",
 *       pipeline:  [{$merge: {into: "target", whenMatched: "merge", whenNotMatched: "insert"}}],
 *       cursor:    {getMergeStats: true}
 *   });
 *
 * When the terminal stage is $merge or $out AND `getMergeStats` is true AND the
 * `MergeStatsHandle` feature flag is enabled on the server, the cursor reply gains a
 * top-level field:
 *
 *   cursor: {
 *       id: 0,
 *       ns: "db.source",
 *       firstBatch: [],
 *       mergeStats: {
 *           stage: "$merge",                  // or "$out"
 *           totalDocsProcessed: <NumberLong>,
 *           matched:    <NumberLong>,
 *           inserted:   <NumberLong>,
 *           replaced:   <NumberLong>,
 *           discarded:  <NumberLong>,
 *           failed:     <NumberLong>
 *       }
 *   }
 *
 * Today, the option is unknown to the server and the reply contains no such field.
 * This test asserts BOTH halves:
 *   (1) absent-by-default behavior is pinned, and
 *   (2) the feature-flag-conditional block sketches the expected behavior once the
 *       counters are wired through and serialized onto the cursor reply.
 *
 * The conditional block is a no-op today: `MergeStatsHandle` does not exist yet, so
 * `FeatureFlagUtil.isPresentAndEnabled(db, "MergeStatsHandle")` returns false on every
 * production binary and the post-implementation assertions are skipped. When the flag
 * lands the same file becomes a real correctness test without any structural changes.
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

const source = db[`${jsTest.name()}_source`];
const target = db[`${jsTest.name()}_target`];
source.drop();
target.drop();

assert.commandWorked(source.insert([
    {_id: 1, k: "a", v: 1},
    {_id: 2, k: "b", v: 2},
    {_id: 3, k: "c", v: 3},
]));
assert.commandWorked(target.insert([
    {_id: 1, k: "a", v: 99},  // will match  -> replaced/merged
    {_id: 4, k: "d", v: 4},   // not touched
]));

const mergePipeline = [{
    $merge: {into: target.getName(), whenMatched: "merge", whenNotMatched: "insert"},
}];

// ---------- Category 1: stats absent on opt-out (CURRENT BEHAVIOR) ----------
(function testMergeStatsAbsentByDefault() {
    const res = assert.commandWorked(db.runCommand({
        aggregate: source.getName(),
        pipeline:  mergePipeline,
        cursor:    {},
    }));
    assert(res.hasOwnProperty("cursor"), "aggregate reply must carry a cursor");
    assert.eq(res.cursor.firstBatch.length, 0, "$merge returns zero documents");
    assert(!res.cursor.hasOwnProperty("mergeStats"),
           "default cursor reply must NOT carry mergeStats today");
})();

// ---------- Category 2: stats absent on opt-out even with explicit false -----
(function testMergeStatsAbsentWhenFalse() {
    target.drop();
    assert.commandWorked(target.insert({_id: 1, k: "a", v: 99}));

    const res = assert.commandWorked(db.runCommand({
        aggregate: source.getName(),
        pipeline:  mergePipeline,
        cursor:    {getMergeStats: false},
    }));
    assert(!res.cursor.hasOwnProperty("mergeStats"),
           "explicit getMergeStats:false must not produce mergeStats");
})();

// ---------- Category 3: proposed shape under feature flag (NO-OP today) -----
(function testMergeStatsPresentWhenRequested_FlagGated() {
    const flagOn = FeatureFlagUtil.isPresentAndEnabled(db, "MergeStatsHandle");
    if (!flagOn) {
        jsTest.log("MergeStatsHandle feature flag absent or disabled — " +
                   "skipping proposed-API assertions. This block becomes active " +
                   "once SERVER-43194's follow-up implementation lands.");
        return;
    }

    target.drop();
    assert.commandWorked(target.insert([
        {_id: 1, k: "a", v: 99},
        {_id: 4, k: "d", v: 4},
    ]));

    const res = assert.commandWorked(db.runCommand({
        aggregate: source.getName(),
        pipeline:  mergePipeline,
        cursor:    {getMergeStats: true},
    }));
    assert(res.cursor.hasOwnProperty("mergeStats"),
           "with the flag on, cursor reply must carry mergeStats");

    const stats = res.cursor.mergeStats;
    assert.eq(stats.stage, "$merge");
    assert.eq(stats.totalDocsProcessed, 3, "all source docs reach the terminal stage");
    assert.eq(stats.matched,   1, "_id:1 matches existing target row");
    assert.eq(stats.inserted,  2, "_id:2 and _id:3 are new in target");
    assert.eq(stats.replaced,  0, "whenMatched:merge does field-merge, not replace");
    assert.eq(stats.discarded, 0, "whenNotMatched:insert never discards");
    assert.eq(stats.failed,    0, "no write errors expected on this input");
})();

// ---------- Category 4: $out coverage under the same handle (NO-OP today) ---
(function testOutStatsPresentWhenRequested_FlagGated() {
    const flagOn = FeatureFlagUtil.isPresentAndEnabled(db, "MergeStatsHandle");
    if (!flagOn) {
        return;  // logged above; do not double-log
    }

    target.drop();
    const res = assert.commandWorked(db.runCommand({
        aggregate: source.getName(),
        pipeline:  [{$out: target.getName()}],
        cursor:    {getMergeStats: true},
    }));
    assert(res.cursor.hasOwnProperty("mergeStats"),
           "$out must populate the same handle as $merge");

    const stats = res.cursor.mergeStats;
    assert.eq(stats.stage, "$out");
    assert.eq(stats.totalDocsProcessed, 3);
    assert.eq(stats.inserted, 3, "$out writes all 3 docs to the temp collection");
    assert.eq(stats.matched,   0, "$out does not match on fields");
    assert.eq(stats.replaced,  0, "$out replaces the collection, not rows");
    assert.eq(stats.discarded, 0);
    assert.eq(stats.failed,    0);
})();

// ---------- Category 5: harmless on non-write pipelines (NO-OP today) -------
(function testGetMergeStatsHarmlessOnReadPipeline_FlagGated() {
    const flagOn = FeatureFlagUtil.isPresentAndEnabled(db, "MergeStatsHandle");
    if (!flagOn) {
        return;
    }

    // Requesting stats on a pipeline without a terminal write stage must not error;
    // mergeStats simply isn't populated. Drivers that always set the flag stay safe.
    const res = assert.commandWorked(db.runCommand({
        aggregate: source.getName(),
        pipeline:  [{$match: {v: {$gte: 1}}}],
        cursor:    {getMergeStats: true},
    }));
    assert(!res.cursor.hasOwnProperty("mergeStats"),
           "non-write pipeline must not synthesize mergeStats");
})();
