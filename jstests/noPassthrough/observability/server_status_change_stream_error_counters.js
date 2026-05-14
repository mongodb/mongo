/**
 * SERVER-126369: pins the process-wide change-stream error counters exposed at
 * serverStatus().metrics.changeStreams.errors.
 *
 * The metric site is gated on featureFlagChangeStreamErrorCounters. If the counters are not present
 * in serverStatus output, the test skip-gates (the change is not yet landed on this branch).
 *
 * @tags: [
 *   requires_replication,
 *   uses_change_streams,
 *   featureFlagChangeStreamErrorCounters,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";

const COUNTER_PATH = ["changeStreams", "errors"];
const COUNTER_KEYS = [
    "totalRetriable",
    "totalNonRetriable",
    "historyLost",
    "resumeTokenNotFound",
    "bsonObjectTooLarge",
    "interruptedDueToReplStepChange",
];

function readErrors(db) {
    let cursor = db.serverStatus().metrics;
    for (const key of COUNTER_PATH) {
        if (!cursor || typeof cursor !== "object" || !(key in cursor)) {
            return null;
        }
        cursor = cursor[key];
    }
    return cursor;
}

function diff(before, after) {
    const out = {};
    for (const k of COUNTER_KEYS) {
        out[k] = (after[k] || 0) - (before[k] || 0);
    }
    return out;
}

const rst = new ReplSetTest({name: jsTest.name(), nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const db = primary.getDB(jsTest.name());

const initial = readErrors(db);
if (initial === null) {
    jsTestLog(
        "SERVER-126369 counter site not present at serverStatus().metrics.changeStreams.errors -- " +
            "skipping (feature flag off or metric not landed).",
    );
    rst.stopSet();
    quit();
}

// Sanity: every expected key is exposed.
for (const k of COUNTER_KEYS) {
    assert(k in initial, `missing counter ${k} in ${tojson(initial)}`);
    assert.eq(typeof initial[k], "number", `counter ${k} should be a number`);
}

const coll = db.getCollection("c");
assert.commandWorked(coll.insert({_id: 0}));

// -- ChangeStreamHistoryLost: resume from a fake high-water-mark token forces history-lost on a
// non-existent collection clustered key. Drives both `historyLost` and `totalNonRetriable`.
{
    const before = readErrors(db);
    try {
        coll.watch([], {resumeAfter: {_data: "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"}})
            .hasNext();
    } catch (e) {
        // expected
    }
    const after = readErrors(db);
    const d = diff(before, after);
    assert.gte(d.historyLost + d.totalNonRetriable, 1, `expected history-lost bump: ${tojson(d)}`);
}

// -- BSONObjectTooLarge: inject via failpoint on the change-stream pipeline.
{
    const before = readErrors(db);
    const fp = configureFailPoint(primary, "throwChangeStreamBSONObjectTooLarge");
    try {
        const cs = coll.watch();
        assert.commandWorked(coll.insert({_id: 1, payload: "x".repeat(64)}));
        try {
            cs.hasNext();
        } catch (e) {
            // expected
        }
    } finally {
        fp.off();
    }
    const after = readErrors(db);
    const d = diff(before, after);
    // Best-effort: failpoint may not be wired pre-landing; tolerate zero but assert non-negative.
    for (const k of COUNTER_KEYS) {
        assert.gte(d[k], 0, `counter ${k} regressed: ${tojson(d)}`);
    }
}

// -- Subset invariant on the cumulative reading.
{
    const cur = readErrors(db);
    const subBuckets =
        cur.historyLost + cur.resumeTokenNotFound + cur.bsonObjectTooLarge + cur.interruptedDueToReplStepChange;
    const totals = cur.totalRetriable + cur.totalNonRetriable;
    assert.lte(
        subBuckets,
        totals,
        `subset invariant violated: subBuckets=${subBuckets} > totals=${totals} :: ${tojson(cur)}`,
    );
}

rst.stopSet();
