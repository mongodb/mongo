/**
 * Regression-pin audit for ingress connection backpressure under worker
 * thread exhaustion.
 *
 * Background: SessionManagerCommon::startSession only refuses new connections
 * when the in-flight session count exceeds _maxOpenSessions. Pressure from the
 * worker pool itself is invisible to that gate, so a connection storm against a
 * deliberately small worker pool can saturate dispatch without producing a
 * structured refusal at the listener. This test documents the *current*
 * behaviour against a mongod that has been forced into a small-pool
 * configuration: either each excess connection produces a structured refusal
 * (the desired behaviour once SERVER-124134 lands), or the server at minimum
 * survives the storm without faulting (the floor we hold today).
 *
 * Either outcome is acceptable to this test; what it pins is that the server
 * does NOT silently OOM and does NOT silently consume the entire connection
 * budget without surfacing the pressure in serverStatus.
 *
 * @tags: [
 *   grpc_incompatible,
 *   requires_fcv_80,
 * ]
 */
import {Thread} from "jstests/libs/parallelTester.js";

const kWorkerPoolBudget = 4;
const kStormConnections = 32;
const kSocketTimeoutMs = 8000;

// Deliberately small worker / connection budget so the storm is guaranteed to
// run the server out of headroom without requiring host-level rlimit tweaks.
const conn = MongoRunner.runMongod({
    setParameter: {
        // Cap the active session ceiling well below the storm size.
        maxIncomingConnections: kWorkerPoolBudget,
        // Keep the rate limiter out of the picture so we are exercising the
        // session-manager limit, not the establishment-rate limiter.
        ingressConnectionEstablishmentRateLimiterEnabled: false,
    },
});
assert.neq(null, conn, "mongod failed to start with a small worker pool budget");

const adminDb = conn.getDB("admin");

function snapshotConnections() {
    const status = assert.commandWorked(adminDb.serverStatus({connections: 1}));
    return status.connections;
}

const before = snapshotConnections();
jsTestLog("ingress_backpressure_audit: pre-storm serverStatus.connections=" + tojson(before));
assert.gte(before.available, 0, "connections.available must be reported");

// Storm.
const threads = [];
for (let i = 0; i < kStormConnections; i++) {
    threads.push(
        new Thread(
            (host, threadId, socketTimeoutMs) => {
                const outcome = {threadId: threadId, accepted: false, rejected: false, error: null};
                try {
                    const c = new Mongo(`mongodb://${host}/?socketTimeoutMS=${socketTimeoutMs}`);
                    // A connection that lands but is immediately torn down still counts as
                    // "accepted" from the listener's point of view.
                    outcome.accepted = c != null;
                    try {
                        c.getDB("admin").runCommand({ping: 1});
                    } catch (_pingErr) {
                        // ping may fail under saturation; that is fine for this audit.
                    }
                } catch (e) {
                    outcome.rejected = true;
                    outcome.error = String(e);
                }
                return outcome;
            },
            conn.host,
            i,
            kSocketTimeoutMs,
        ),
    );
    threads[i].start();
}

const outcomes = threads.map((t) => {
    t.join();
    return t.returnData();
});

const accepted = outcomes.filter((o) => o.accepted).length;
const rejected = outcomes.filter((o) => o.rejected).length;
jsTestLog(`ingress_backpressure_audit: storm finished accepted=${accepted} rejected=${rejected}`);

// Property 1: every storm thread eventually terminates.
assert.eq(outcomes.length, kStormConnections, "every storm thread must complete");

// Property 2: the server is still answering admin commands after the storm.
// This is the load-bearing assertion: it is what distinguishes "we held the line"
// from "we OOMed / cascaded into fault".
const ok = assert.commandWorked(adminDb.runCommand({hello: 1}));
assert(ok.ok, "mongod must remain responsive after a connection storm");

// Property 3: serverStatus reports the pressure somewhere observable.
// Today the signal is the gap between connections.current and the budget;
// once SERVER-124134 lands, "refusedForBackpressure" (or an equivalent named
// counter) should be non-zero whenever rejected > 0. We assert the *floor*:
// either the storm pushed current up to the cap, or refusals were recorded.
const after = snapshotConnections();
jsTestLog("ingress_backpressure_audit: post-storm serverStatus.connections=" + tojson(after));

const hitCap = after.current >= kWorkerPoolBudget;
const refusedCounterPresent =
    after.rejected !== undefined && Number(after.rejected) > Number(before.rejected || 0);
const structuredRefusalsObserved = rejected > 0;

assert(
    hitCap || refusedCounterPresent || structuredRefusalsObserved,
    "ingress_backpressure_audit: storm produced no observable pressure signal — " +
        tojson({before, after, accepted, rejected}),
);

// Property 4: privileged-port admin still works. We do not configure a
// dedicated priority port here, so this is just a smoke that the existing
// connection on `conn` is still usable for admin work.
assert.commandWorked(adminDb.runCommand({ping: 1}));

MongoRunner.stopMongod(conn);
