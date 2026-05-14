/**
 * SERVER-120876: oplog-volume-triggered checkpoint cadence.
 *
 * Exercises the three new server parameters introduced by SERVER-120876:
 *   - checkpointMinIntervalSecs
 *   - checkpointMaxIntervalSecs
 *   - checkpointOplogVolumeBytes
 *
 * Verifies the matrix:
 *   (1) max-interval ceiling fires a checkpoint with zero oplog writes.
 *   (2) volume gate fires a checkpoint before the max interval when enough
 *       oplog data has accumulated.
 *   (3) min-interval floor suppresses back-to-back checkpoints under burst.
 *   (4) explicit syncdelay override (sentinel cleared) bypasses the volume gate.
 *   (5) checkpointOplogVolumeBytes == 0 falls back to legacy cadence.
 *   (6) min > max is rejected at parameter-set time.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 *   requires_wiredtiger,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const minSecs = 2;
const maxSecs = 10;
const volumeBytes = 1 * 1024 * 1024; // 1 MiB — small to keep test wall time low.

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            checkpointMinIntervalSecs: minSecs,
            checkpointMaxIntervalSecs: maxSecs,
            checkpointOplogVolumeBytes: volumeBytes,
        },
    },
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const adminDB = primary.getDB("admin");
const testDB = primary.getDB("checkpoint_oplog_volume");
const coll = testDB.c;

function checkpointGeneration() {
    const wt = adminDB.serverStatus().wiredTiger;
    assert(wt, "wiredTiger section missing from serverStatus");
    return wt.checkpoint.generation;
}

function waitForGenerationAdvance(prior, timeoutMs) {
    let observed = prior;
    assert.soon(
        () => {
            observed = checkpointGeneration();
            return observed > prior;
        },
        () => `expected checkpoint generation to advance past ${prior}, still ${observed}`,
        timeoutMs,
        500);
    return observed;
}

function insertBytes(targetBytes) {
    // ~1 KiB payload per doc.
    const payload = "x".repeat(1000);
    const docsNeeded = Math.ceil(targetBytes / 1000) + 4;
    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < docsNeeded; ++i) {
        bulk.insert({_id: new Date().getTime() + "_" + i, p: payload});
    }
    assert.commandWorked(bulk.execute());
}

// ------------------------------------------------------------------
// Case 1: max-interval ceiling fires a checkpoint with no oplog writes.
// ------------------------------------------------------------------
jsTestLog("case 1: max-interval ceiling fires with no oplog writes");
{
    // Drain any pending generation tick.
    sleep(minSecs * 1000);
    const baseline = checkpointGeneration();
    // No writes. Wait slightly past max-interval and assert we advanced.
    const after = waitForGenerationAdvance(baseline, (maxSecs + minSecs + 5) * 1000);
    jsTestLog(`case 1 ok: generation ${baseline} -> ${after}`);
}

// ------------------------------------------------------------------
// Case 2: volume gate fires before max-interval.
// ------------------------------------------------------------------
jsTestLog("case 2: volume gate fires before max-interval");
{
    sleep(minSecs * 1000);
    const baseline = checkpointGeneration();
    const t0 = Date.now();
    insertBytes(volumeBytes * 2); // safely above threshold
    const after = waitForGenerationAdvance(baseline, (maxSecs - 1) * 1000);
    const elapsed = Date.now() - t0;
    assert.lt(elapsed,
              maxSecs * 1000,
              `volume-triggered checkpoint should have fired before max-interval; elapsed=${elapsed}ms`);
    jsTestLog(`case 2 ok: generation ${baseline} -> ${after} in ${elapsed}ms`);
}

// ------------------------------------------------------------------
// Case 3: min-interval floor suppresses back-to-back checkpoints.
// ------------------------------------------------------------------
jsTestLog("case 3: min-interval floor holds under burst");
{
    // Force a checkpoint as our reference point.
    assert.commandWorked(adminDB.runCommand({fsync: 1}));
    const baseline = checkpointGeneration();
    // Drive several volume-thresholds back-to-back; min-interval should keep
    // us to at most one additional checkpoint inside that window.
    const burstStart = Date.now();
    for (let i = 0; i < 4; ++i) {
        insertBytes(volumeBytes * 2);
    }
    // Wait less than min-interval and confirm we have not exceeded baseline+1.
    sleep(Math.floor(minSecs * 1000 * 0.5));
    const mid = checkpointGeneration();
    assert.lte(mid,
               baseline + 1,
               `inside min-interval window, expected at most one new checkpoint, saw ${mid - baseline}`);
    jsTestLog(`case 3 ok: held to ${mid - baseline} new checkpoint(s) inside min-interval`);
    // Drain so subsequent cases start from a steady state.
    sleep((minSecs + 1) * 1000);
}

// ------------------------------------------------------------------
// Case 4: explicit syncdelay override bypasses the volume gate.
// ------------------------------------------------------------------
jsTestLog("case 4: explicit syncdelay override bypasses volume gate");
{
    // Set syncdelay to a value larger than max-interval — if the override is
    // honoured, the next checkpoint should follow syncdelay, not max-interval.
    const overrideSyncdelay = maxSecs * 3;
    assert.commandWorked(
        adminDB.runCommand({setParameter: 1, syncdelay: overrideSyncdelay}));

    // Force a clean reference checkpoint.
    assert.commandWorked(adminDB.runCommand({fsync: 1}));
    const baseline = checkpointGeneration();

    // Drive volume past the gate.
    insertBytes(volumeBytes * 2);

    // Wait past max-interval but well under override syncdelay.
    sleep((maxSecs + 2) * 1000);
    const mid = checkpointGeneration();
    assert.eq(mid,
              baseline,
              `volume gate must be bypassed when syncdelay override is set; saw ${mid - baseline} new checkpoints`);
    jsTestLog(`case 4 ok: no checkpoint inside ${maxSecs + 2}s with syncdelay=${overrideSyncdelay}`);

    // Restore default so case 5 starts clean.
    assert.commandWorked(adminDB.runCommand({setParameter: 1, syncdelay: -1.0}));
}

// ------------------------------------------------------------------
// Case 5: checkpointOplogVolumeBytes == 0 falls back to legacy cadence.
// ------------------------------------------------------------------
jsTestLog("case 5: volume=0 falls back to legacy interval-only cadence");
{
    assert.commandWorked(
        adminDB.runCommand({setParameter: 1, checkpointOplogVolumeBytes: 0}));

    assert.commandWorked(adminDB.runCommand({fsync: 1}));
    const baseline = checkpointGeneration();

    // Drive a write storm. With volume gate disabled, we should NOT see a
    // sub-max-interval checkpoint.
    insertBytes(volumeBytes * 4);
    sleep((minSecs + 1) * 1000);
    const mid = checkpointGeneration();
    assert.eq(mid,
              baseline,
              `with volume=0, no volume-driven checkpoint expected; saw ${mid - baseline}`);

    // But the max-interval ceiling must still trip eventually.
    const after = waitForGenerationAdvance(baseline, (maxSecs + minSecs + 5) * 1000);
    jsTestLog(`case 5 ok: legacy cadence held; eventual generation ${baseline} -> ${after}`);

    // Restore default.
    assert.commandWorked(
        adminDB.runCommand({setParameter: 1, checkpointOplogVolumeBytes: volumeBytes}));
}

// ------------------------------------------------------------------
// Case 6: validator rejects min > max at runtime.
// ------------------------------------------------------------------
jsTestLog("case 6: validator rejects min > max");
{
    // Bump max down toward min, then attempt to push min above it.
    assert.commandWorked(
        adminDB.runCommand({setParameter: 1, checkpointMaxIntervalSecs: minSecs + 1}));
    const bad = adminDB.runCommand({
        setParameter: 1,
        checkpointMinIntervalSecs: minSecs + 5, // > max
    });
    assert.commandFailedWithCode(bad, ErrorCodes.BadValue);

    // Restore sane state.
    assert.commandWorked(
        adminDB.runCommand({setParameter: 1, checkpointMaxIntervalSecs: maxSecs}));
    assert.commandWorked(
        adminDB.runCommand({setParameter: 1, checkpointMinIntervalSecs: minSecs}));
    jsTestLog("case 6 ok: validator rejected min > max with BadValue");
}

rst.stopSet();
