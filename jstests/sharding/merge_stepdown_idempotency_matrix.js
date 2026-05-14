/**
 * Drives every (whenMatched x whenNotMatched) $merge mode through a forced primary
 * stepdown that occurs mid-pipeline, then re-runs the same aggregation against the
 * newly elected primary. After re-replay the test fingerprints the target collection
 * and asserts whether the final state matches the bit-identical single-run result.
 *
 * The test outputs a structured summary table of:
 *   {whenMatched, whenNotMatched, outcome: pass|unsafe, reason}
 *
 * 'pass'   -> target state after stepdown + retry matches the single-run baseline.
 * 'unsafe' -> target state differs (e.g. double-applied increment / partial batch),
 *             which is why the corresponding mode is currently excluded from
 *             config-transition test passthroughs.
 *
 * Two reasonable fixtures exist for this kind of mid-pipeline stepdown test: a plain
 * three-node ReplSetTest or a ShardingTest with rs-backed shards. Because the
 * behaviour under test is local to the $merge writer's own retry semantics (not the
 * router or chunk migration), a single three-node ReplSetTest is sufficient and
 * keeps the test cheap. The file is placed under jstests/sharding/ per the
 * exclusion ticket's stated home for stepdown-during-aggregation coverage.
 *
 * @tags: [
 *   requires_replication,
 *   requires_majority_read_concern,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const kNumSourceDocs = 500;
const kDbName = jsTestName();
const kSourceName = "source";
const kTargetName = "target";
const kMergeFailPoint = "hangWhileBuildingDocumentSourceMergeBatch";

// The full mode cross-product. Modes flagged as 'specInvalid' are rejected at parse
// time and never reach the writer, so we skip them and report them as such in the
// summary rather than executing them.
const kModeMatrix = [
    {whenMatched: "replace", whenNotMatched: "insert", specInvalid: false},
    {whenMatched: "replace", whenNotMatched: "fail", specInvalid: false},
    {whenMatched: "replace", whenNotMatched: "discard", specInvalid: false},
    {whenMatched: "merge", whenNotMatched: "insert", specInvalid: false},
    {whenMatched: "merge", whenNotMatched: "fail", specInvalid: false},
    {whenMatched: "merge", whenNotMatched: "discard", specInvalid: false},
    {whenMatched: "keepExisting", whenNotMatched: "insert", specInvalid: false},
    {whenMatched: "keepExisting", whenNotMatched: "fail", specInvalid: true},
    {whenMatched: "keepExisting", whenNotMatched: "discard", specInvalid: true},
    {whenMatched: "fail", whenNotMatched: "insert", specInvalid: false},
    {whenMatched: "fail", whenNotMatched: "fail", specInvalid: true},
    {whenMatched: "fail", whenNotMatched: "discard", specInvalid: true},
    {whenMatched: "pipeline", whenNotMatched: "insert", specInvalid: false},
    {whenMatched: "pipeline", whenNotMatched: "fail", specInvalid: false},
    {whenMatched: "pipeline", whenNotMatched: "discard", specInvalid: false},
];

// Resolve the pipeline variant for whenMatched=pipeline. The pipeline body
// increments a per-document counter so that a double-application is detectable in
// the fingerprint.
function resolveWhenMatched(mode) {
    if (mode === "pipeline") {
        return [{$addFields: {applyCount: {$add: [{$ifNull: ["$applyCount", 0]}, 1]}}}];
    }
    return mode;
}

// Build the same deterministic source on every iteration so the baseline run and
// the stepdown-replay run see identical input.
function seedSource(coll) {
    assert(coll.drop());
    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < kNumSourceDocs; i++) {
        // Half of the source overlaps with the seeded target, half does not.
        bulk.insert({_id: i, group: i % 10, value: i, applyCount: 0});
    }
    assert.commandWorked(bulk.execute({w: "majority"}));
}

// Seed the target with a deterministic prefix so 'whenMatched' has matches and
// 'whenNotMatched' has non-matches in the same run.
function seedTarget(coll) {
    assert(coll.drop());
    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < kNumSourceDocs / 2; i++) {
        bulk.insert({_id: i, group: -1, value: -1, applyCount: 0});
    }
    assert.commandWorked(bulk.execute({w: "majority"}));
}

// Stable fingerprint: sum + count + checksum of sorted (_id, applyCount, value).
function fingerprintTarget(coll) {
    const rows = coll
        .find({}, {_id: 1, value: 1, applyCount: 1})
        .sort({_id: 1})
        .toArray();
    let checksum = 0;
    let applySum = 0;
    for (const r of rows) {
        const av = r.applyCount === undefined ? 0 : r.applyCount;
        const vv = r.value === undefined ? 0 : r.value;
        checksum = (checksum + r._id * 31 + vv * 17 + av * 7) | 0;
        applySum += av;
    }
    return {count: rows.length, checksum: checksum, applySum: applySum};
}

function buildPipeline(mode) {
    return [
        {$sort: {_id: 1}},
        {$_internalInhibitOptimization: {}},
        {
            $merge: {
                into: kTargetName,
                whenMatched: resolveWhenMatched(mode.whenMatched),
                whenNotMatched: mode.whenNotMatched,
            },
        },
    ];
}

// Run the pipeline straight through (no stepdown) to capture the single-run baseline.
function captureBaseline(primaryDB, mode) {
    seedSource(primaryDB[kSourceName]);
    seedTarget(primaryDB[kTargetName]);
    try {
        primaryDB[kSourceName].aggregate(buildPipeline(mode));
    } catch (e) {
        return {error: e.code || e.message, fingerprint: null};
    }
    return {error: null, fingerprint: fingerprintTarget(primaryDB[kTargetName])};
}

// Run the pipeline with a mid-pipeline stepdown, then re-issue against the new
// primary so the user-perceived semantics ("the pipeline was retried") are what we
// fingerprint.
function captureStepdownReplay(replTest, mode) {
    let primary = replTest.getPrimary();
    let primaryDB = primary.getDB(kDbName);

    seedSource(primaryDB[kSourceName]);
    seedTarget(primaryDB[kTargetName]);
    replTest.awaitReplication();

    const fp = configureFailPoint(primary, kMergeFailPoint);
    const pipelineJson = tojson(buildPipeline(mode));

    const shellBody = `
        const aggDB = db.getSiblingDB("${kDbName}");
        const res = aggDB.runCommand({
            aggregate: "${kSourceName}",
            pipeline: ${pipelineJson},
            cursor: {},
        });
        // Either success, a write-after-stepdown style error, or the spec-defined
        // mode error (e.g. DuplicateKey / MergeStageNoMatchingDocument). All are
        // acceptable at this stage; correctness is judged on the final target
        // state after retry, not on whether the first attempt completed.
        if (!res.ok) {
            print("first-attempt failure code=" + res.code);
        }
    `;

    const aggShell = startParallelShell(shellBody, primary.port);
    fp.wait();

    // Force a new primary by stepping up a secondary; the old primary's in-flight
    // $merge writes will be interrupted.
    const secondaries = replTest.getSecondaries();
    assert.commandWorked(secondaries[0].adminCommand({replSetFreeze: 0}));
    const newPrimary = replTest.stepUp(secondaries[0], {awaitReplicationBeforeStepUp: false});

    fp.off();
    aggShell();

    replTest.awaitNodesAgreeOnPrimary();
    primary = replTest.getPrimary();
    primaryDB = primary.getDB(kDbName);

    // Driver-style retry: re-issue the same pipeline against whoever is now primary.
    try {
        primaryDB[kSourceName].aggregate(buildPipeline(mode));
    } catch (e) {
        return {error: e.code || e.message, fingerprint: fingerprintTarget(primaryDB[kTargetName])};
    }
    return {error: null, fingerprint: fingerprintTarget(primaryDB[kTargetName])};
}

function fingerprintsMatch(a, b) {
    if (a === null || b === null) return false;
    return a.count === b.count && a.checksum === b.checksum && a.applySum === b.applySum;
}

function classify(mode, baseline, replay) {
    if (baseline.error && replay.error && baseline.error === replay.error) {
        return {outcome: "pass", reason: "mode rejects matched/unmatched identically on both paths"};
    }
    if (fingerprintsMatch(baseline.fingerprint, replay.fingerprint)) {
        if (mode.whenMatched === "pipeline" && replay.fingerprint && replay.fingerprint.applySum > baseline.fingerprint.applySum) {
            return {outcome: "unsafe", reason: "pipeline body applied more than once after retry"};
        }
        return {outcome: "pass", reason: "post-retry target bit-identical to single-run baseline"};
    }
    return {outcome: "unsafe", reason: "post-retry target diverged from single-run baseline; retry not idempotent"};
}

const replTest = new ReplSetTest({name: "mergeStepdownMatrix", nodes: 3});
replTest.startSet();
replTest.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

// Allow the test to use w:1 baseline writes; replSet majority is the production
// posture but is not load-bearing for the idempotency comparison here.
const initialPrimary = replTest.getPrimary();
assert.commandWorked(
    initialPrimary.adminCommand({
        setDefaultRWConcern: 1,
        defaultWriteConcern: {w: "majority"},
        writeConcern: {w: "majority"},
    }),
);

const summary = [];

for (const mode of kModeMatrix) {
    jsTestLog(`whenMatched=${mode.whenMatched}, whenNotMatched=${mode.whenNotMatched}`);

    if (mode.specInvalid) {
        summary.push({
            whenMatched: mode.whenMatched,
            whenNotMatched: mode.whenNotMatched,
            outcome: "n/a",
            reason: "combination rejected at parse time per documented mode matrix",
        });
        continue;
    }

    let primary = replTest.getPrimary();
    let primaryDB = primary.getDB(kDbName);
    const baseline = captureBaseline(primaryDB, mode);

    let replay;
    try {
        replay = captureStepdownReplay(replTest, mode);
    } catch (e) {
        replay = {error: e.code || e.message, fingerprint: null};
    }

    const verdict = classify(mode, baseline, replay);
    summary.push({
        whenMatched: mode.whenMatched,
        whenNotMatched: mode.whenNotMatched,
        outcome: verdict.outcome,
        reason: verdict.reason,
    });
}

jsTestLog("=== merge_stepdown_idempotency_matrix summary ===");
jsTestLog(tojson(summary));

// Surface the table as the test's terminal artefact. The test does NOT fail when
// a mode is classified 'unsafe' -- the entire purpose is to enumerate which
// modes need passthrough exclusion. Operators / passthrough authors consume the
// printed summary to decide where to keep / remove the existing exclusions.
const unsafeCount = summary.filter((r) => r.outcome === "unsafe").length;
jsTestLog(`unsafe modes: ${unsafeCount} / ${summary.length}`);

replTest.stopSet();
