/**
 * SERVER-115952: Page Server Reader (PSR) backoff/retry must treat read errors
 * the same way it treats connection errors. Prior to the fix, the timeout path
 * in PageServerReplicaSet incremented a separate `_readFailures` counter that
 * was visible in serverStatus but was NOT consumed by BackoffState. The result
 * was an unbounded retry loop against a lagged page-server cell (HELP-86312).
 *
 * This test:
 *   1. Boots a single mongod with disagg enabled (skips gracefully if not).
 *   2. Injects a synthetic read error via the `pageServerInjectReadError`
 *      failpoint (skips gracefully if the failpoint is not compiled in).
 *   3. Drives a bounded number of reads.
 *   4. Asserts that the BackoffState reaches an "escalated" zone within the
 *      retry budget, rather than the metric showing infinite identical
 *      retries with no backoff progress.
 *
 * The bug is specifically that `_readFailures++` does not advance backoff;
 * the fix unifies it with `_connectionFailures` into a single
 * `_requestFailures` counter that BackoffState reads. The assertion below
 * encodes the post-fix invariant: after N injected read errors, either the
 * unified failure counter or the backoff-level metric must reflect them.
 *
 * @tags: [
 *   requires_fcv_81,
 *   featureFlagDisaggregatedStorage,
 * ]
 */

// ---- 1. Bring up a single mongod. -----------------------------------------
const conn = MongoRunner.runMongod({});
if (!conn) {
    jsTest.log.info("Skipping: could not start mongod");
    quit();
}
const adminDB = conn.getDB("admin");
const testDB = conn.getDB("psr_backoff_read_failures");

// ---- 2. Disagg-availability gate. -----------------------------------------
// PSR only exists in disagg builds. If the server doesn't advertise itself
// as disagg-capable via persistenceProviderProperties, skip cleanly.
function isDisaggBuild() {
    const res = adminDB.runCommand({persistenceProviderProperties: 1});
    if (!res.ok) {
        return false;
    }
    // Different disagg flavors expose different keys; treat any of them as
    // sufficient evidence that PSR is wired in.
    return Boolean(
        res.supportsColdCollections ||
            res.supportsPageServer ||
            res.providerName === "disaggregated" ||
            res.providerKind === "disagg",
    );
}

if (!isDisaggBuild()) {
    jsTest.log.info(
        "Skipping page_server_read_error_counts_as_failure: this build does not advertise disaggregated storage",
    );
    MongoRunner.stopMongod(conn);
    quit();
}

// ---- 3. Failpoint-availability gate. --------------------------------------
// The failpoint name follows the existing PSR convention; if a different
// build calls it something else, the test skips rather than hard-failing,
// because the bug itself is build-conditional.
const kInjectReadErrorFP = "pageServerInjectReadError";
const fpRes = adminDB.runCommand({
    configureFailPoint: kInjectReadErrorFP,
    mode: "alwaysOn",
    data: {errorCode: ErrorCodes.NetworkInterfaceExceededTimeLimit},
});
if (!fpRes.ok) {
    jsTest.log.info("Skipping: failpoint '" + kInjectReadErrorFP + "' not present in this build (code " + fpRes.code +
                    ", errmsg: " + tojson(fpRes.errmsg) + ")");
    MongoRunner.stopMongod(conn);
    quit();
}

// ---- 4. Capture the baseline backoff state. -------------------------------
function pageServerBackoffSnapshot() {
    const ss = adminDB.serverStatus();
    // Disagg-aware serverStatus surfaces backoff state under
    // {disagg: {pageServer: {backoff: {...}, errors: {...}}}}. Defensive
    // navigation: missing sub-doc means "feature not present" or
    // "no recorded activity yet", both of which we treat as zeros.
    const ps = (ss && ss.disagg && ss.disagg.pageServer) || {};
    return {
        readErrors: (ps.errors && ps.errors.readErrors) || 0,
        connectionErrors: (ps.errors && ps.errors.connectionErrors) || 0,
        requestFailures: (ps.backoff && ps.backoff.requestFailures) || 0,
        escalations: (ps.backoff && ps.backoff.escalations) || 0,
        currentDelayMs: (ps.backoff && ps.backoff.currentDelayMs) || 0,
    };
}

const before = pageServerBackoffSnapshot();
jsTest.log.info("PSR backoff baseline: " + tojson(before));

// ---- 5. Drive a bounded number of reads. ----------------------------------
// We don't care if individual reads succeed or fail at the client layer;
// we care that PSR observed N read errors and that the backoff machinery
// reflects them. A bounded loop (vs while(true)) is itself the test of the
// fix: pre-fix, the retry budget would never be exhausted because backoff
// never advanced.
const kReadAttempts = 25;
assert.commandWorked(testDB.createCollection("c"));
for (let i = 0; i < kReadAttempts; ++i) {
    // Any read path will do; we use a trivial find. Pre-fix, every one of
    // these turns into a 30s timeout that does not advance backoff.
    // Post-fix, BackoffState escalates after the first handful, so most
    // attempts return faster (failed-fast) or surface a clear NoMatchingPeer
    // style error rather than hanging.
    try {
        testDB.c.find({}).limit(1).toArray();
    } catch (_e) {
        // Swallow — we're poking the backoff machinery, not asserting on
        // the user-facing outcome.
    }
}

const after = pageServerBackoffSnapshot();
jsTest.log.info("PSR backoff after injected read errors: " + tojson(after));

// ---- 6. The actual assertion encoding the SERVER-115952 fix. --------------
// Either of the following must hold post-fix:
//   (a) `requestFailures` advanced by at least 1 (the unified counter
//       proves the read-error path now feeds BackoffState), OR
//   (b) `escalations` advanced (the backoff state actually rotated), OR
//   (c) `currentDelayMs` is now non-zero (the backoff timer is active).
//
// Pre-fix, all three would stay at their baseline values while
// `readErrors` climbed monotonically — that's exactly the symptom this
// ticket fixes. We additionally require that `readErrors` did climb, to
// confirm the failpoint actually fired.
assert.gt(
    after.readErrors,
    before.readErrors,
    "failpoint did not inject any read errors — test is inconclusive",
);

const advanced =
    after.requestFailures > before.requestFailures ||
    after.escalations > before.escalations ||
    after.currentDelayMs > before.currentDelayMs;

assert(
    advanced,
    "PSR observed " +
        (after.readErrors - before.readErrors) +
        " read errors but BackoffState did not advance — SERVER-115952 regression. " +
        "Snapshot before=" +
        tojson(before) +
        " after=" +
        tojson(after),
);

// ---- 7. Cleanup. ----------------------------------------------------------
assert.commandWorked(adminDB.runCommand({configureFailPoint: kInjectReadErrorFP, mode: "off"}));
MongoRunner.stopMongod(conn);
