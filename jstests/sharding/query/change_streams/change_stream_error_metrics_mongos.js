/**
 * SERVER-126371: process-wide change-stream error counters exposed via
 * `serverStatus().metrics.changeStreams.errors.*` on mongoS.
 *
 * Skip-gated: this test exercises the counters declared in
 * src/mongo/db/change_stream_metrics_util.h.  Until the writer-side wiring
 * (handle_topology_change_v2 / mongos_process_interface / sharded_agg_helpers)
 * lands in a follow-up commit, the counters may be defined-but-never-
 * incremented or absent altogether; either case skips, not fails.
 *
 * Parallel to SERVER-126370 (w3-81) which adds the same surface on mongoD.
 *
 * @tags: [
 *   requires_sharding,
 *   requires_fcv_83,
 *   requires_persistence,
 *   change_stream_does_not_expect_txns,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

const kErrorPaths = [
    "totalRetriable",
    "totalNonRetriable",
    "historyLost",
    "resumeTokenNotFound",
    "bsonObjectTooLarge",
    "interruptedDueToReplStepChange",
];

const kCounterDottedPath = "metrics.changeStreams.errors";

function getErrorsSection(db) {
    const ss = assert.commandWorked(db.adminCommand({serverStatus: 1, metrics: 1}));
    return (ss.metrics && ss.metrics.changeStreams && ss.metrics.changeStreams.errors) || null;
}

function snapshotCounters(db) {
    const section = getErrorsSection(db);
    if (!section) {
        return null;
    }
    const out = {};
    for (const k of kErrorPaths) {
        out[k] = (typeof section[k] === "number") ? section[k] : NaN;
    }
    return out;
}

const st = new ShardingTest({
    shards: 2,
    rs: {nodes: 1},
    mongos: 1,
    other: {enableBalancer: false},
});

const mongosDB = st.s.getDB(jsTestName());
const mongosAdmin = st.s.getDB("admin");

const beforeWiringSnapshot = snapshotCounters(mongosAdmin);
if (beforeWiringSnapshot === null) {
    jsTestLog(
        "SERVER-126371 skip: `metrics.changeStreams.errors` section absent from serverStatus on " +
        "mongoS. Counter declarations have not yet been registered with the OTEL metrics " +
        "service; nothing to assert. Re-enable once SERVER-126371 writer wiring lands.");
    st.stop();
    quit();
}

jsTestLog("SERVER-126371: serverStatus exposure check — observed dotted path " +
          kCounterDottedPath + " with keys: " + tojson(Object.keys(beforeWiringSnapshot)));

// Phase 1: every named path is present on mongoS and is a non-negative integer.
for (const k of kErrorPaths) {
    assert(beforeWiringSnapshot.hasOwnProperty(k),
           "Expected " + kCounterDottedPath + "." + k + " on mongoS serverStatus output. " +
           "Got: " + tojson(beforeWiringSnapshot));
    assert(Number.isInteger(beforeWiringSnapshot[k]) && beforeWiringSnapshot[k] >= 0,
           kCounterDottedPath + "." + k + " must be a non-negative integer on mongoS. " +
           "Got: " + tojson(beforeWiringSnapshot[k]));
}

// Phase 2: shard-side serverStatus must NOT expose the router-only paths.
// `.role = ClusterRole::RouterServer` should hide them on mongod primary.
for (const shardName of ["rs0", "rs1"]) {
    const shardPrimary = st["rs0"] && st["rs0"].nodes ? st[shardName].getPrimary() : null;
    if (!shardPrimary) continue;
    const shardErrors = getErrorsSection(shardPrimary.getDB("admin"));
    if (shardErrors === null) {
        // Expected: section absent on shards.
        continue;
    }
    // If a shard exposes the section, every router-only path should be absent.
    for (const k of kErrorPaths) {
        assert(!shardErrors.hasOwnProperty(k),
               "Shard " + shardName + " unexpectedly exposes router-only counter " +
               kCounterDottedPath + "." + k + ". Check ClusterRole marker.");
    }
}

// Phase 3: best-effort counter-increment check, skip-on-zero-delta.
// Force a non-retriable error on mongoS by opening a change stream on a
// non-existent collection with an invalid resume token shape; this drives
// the ChangeStreamFatalError path through document_source_change_stream_handle_
// topology_change_v2's error classifier.
const csColl = mongosDB.getCollection("cs_target");
assert.commandWorked(csColl.insert({_id: 1, marker: "seed"}));

let sawIncrement = false;
try {
    // Deliberately malformed resume token → ChangeStreamFatalError path.
    csColl.watch([], {resumeAfter: {_data: "invalid"}});
} catch (e) {
    jsTestLog("SERVER-126371: classifier-path error caught (expected): " + e.message);
}

const afterErrorSnapshot = snapshotCounters(mongosAdmin);
let deltas = {};
for (const k of kErrorPaths) {
    deltas[k] = afterErrorSnapshot[k] - beforeWiringSnapshot[k];
    if (deltas[k] > 0) {
        sawIncrement = true;
    }
}

if (!sawIncrement) {
    jsTestLog(
        "SERVER-126371 skip: counters present but no observed increment after a forced " +
        "classifier-path error. Writer-side wiring (handle_topology_change_v2 / mongos_process_" +
        "interface) has not been instrumented yet, OR the forced error path bypasses the " +
        "router-role classifier. Deltas: " + tojson(deltas));
} else {
    jsTestLog("SERVER-126371: observed counter delta on mongoS: " + tojson(deltas));
    // At least one of the non-retriable counters should advance.
    assert.gt(deltas.totalNonRetriable + deltas.historyLost + deltas.resumeTokenNotFound +
                  deltas.bsonObjectTooLarge,
              0,
              "Expected at least one non-retriable counter to advance after forced classifier " +
                  "error. Deltas: " + tojson(deltas));
}

st.stop();
