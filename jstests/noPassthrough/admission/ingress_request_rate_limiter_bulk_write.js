/**
 * Tests how an IRRL 462 (IngressRequestRateLimitExceeded) propagates through a sharded bulkWrite:
 * whole-command rejection at the mongos boundary, a subset of the fan-out rejected (ordered and
 * unordered), and the mongos overload retry cap. The UWE forwards each shard's sub-batch as its own
 * `bulkWrite`, so a sub-batch error is injected/observed on that shard's `bulkWrite`.
 *
 * Scenario 1 exercises a genuine IRRL rejection at the mongos boundary (real token-bucket
 * exhaustion). Scenario 10 exercises a genuine IRRL rejection on a shard -- reached the normal way,
 * through a sharded bulkWrite routed via mongos -- to confirm a real (non-injected) shard-side
 * rejection propagates through the full stack into the same per-op 462 shape Scenarios 2-9 assume.
 * Scenarios 2-9 use `failCommand` (via enableFailCommandOnShards) to reproduce that shape on a
 * shard, because they need to deterministically target one specific shard's specific sub-batch/
 * command -- the real per-node token bucket is shared with unrelated cluster traffic and can't be
 * scoped that precisely without flakiness. What's under test in those scenarios is the
 * propagation/retry/durability behavior downstream of the error, not the token bucket itself;
 * Scenario 10 is the canary that would catch a real shard-side rejection ceasing to propagate the
 * way those scenarios assume.
 *
 * @tags: [requires_fcv_83]
 */

import {cursorEntryValidator, summaryFieldsValidator} from "jstests/libs/bulk_write_utils.js";
import {after, afterEach, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    assertContainsExpectedErrorLabels,
    assertShardingStatisticsDiffEq,
    authenticateConnection,
    disableFailCommandOnShards,
    disableRateLimiter,
    enableFailCommandOnShards,
    enableZeroBurstRateLimiter,
    getShardStats,
    kInternalConnectionAppNameExemptions,
    kKeyFile,
    kRateLimiterExemptAppName,
    kSlowestRefreshRateSecs,
    setParameter,
    setupAuth,
    shardingStatisticsDifference,
} from "jstests/noPassthrough/admission/libs/ingress_request_rate_limiter_helper.js";

const kDb = "bulk_write_irrl_test";
const kColl = "c";
// A second collection sharded on a mutable field, for the shard-key-changing-update scenario
// (_id is immutable, so kColl's {_id:1} key cannot exercise an owning-shard change).
const kShardKeyColl = "skc";

const k462 = ErrorCodes.IngressRequestRateLimitExceeded;

// Number of tokens Scenario 1 drains from the real IRRL before it starts rejecting requests.
const kMaxBurstRequests = 5;

// The exemption matcher treats a client as exempt if any entry here prefix-matches EITHER its
// appName or its driver name (see AppNameExemptionMatcher). Every mongo-shell-created connection --
// including Scenario 1's deliberately non-exempt client, and mongos's own internal connections to
// each shard (e.g. Scenario 10's fan-out) -- reports driver name "MongoDB Internal Client", so that
// entry must be dropped from the shared internal-connection exemption list, or those connections
// would be exempted by their driver name regardless of their distinct appName. The exempt
// connections (exemptConn, and cluster-infrastructure traffic covered by the remaining entries)
// stay exempt via their appName (kRateLimiterExemptAppName), not their driver name, so this
// doesn't affect them.
const kSafeInternalExemptions = [
    kRateLimiterExemptAppName,
    ...kInternalConnectionAppNameExemptions.filter((name) => name !== "MongoDB Internal Client"),
];

describe("bulkWrite IRRL error propagation (budget exhaustion + subset rejection + overload retry cap)", function () {
    let st;
    let exemptConn;
    let ns;
    let skNs;
    let shard0Name;
    let shard1Name;
    let baselineRetryParams;

    // The fan-out forwards each shard's sub-batch as its own `bulkWrite`and bulkWrite runs against
    // the admin db, so use that for the namespace filter.
    function enableOverloadFailCommand(host, times) {
        enableFailCommandOnShards(host, {times}, ["bulkWrite"], "admin");
    }

    // Resolve fresh rather than caching, so a stepdown can't leave the failCommand on a now-secondary.
    function shard0PrimaryHost() {
        return st.rs0.getPrimary().host;
    }

    function shard1PrimaryHost() {
        return st.rs1.getPrimary().host;
    }

    function diskDocs() {
        return exemptConn.getDB(kDb)[kColl].find({}, {_id: 1}).sort({_id: 1}).toArray();
    }

    // Like diskDocs() but also returns the `v` field, so a broadcast update's partial application
    // (which shard touched which docs) is observable.
    function diskDocsWithV() {
        return exemptConn.getDB(kDb)[kColl].find({}, {_id: 1, v: 1}).sort({_id: 1}).toArray();
    }

    // Seed one doc per _id via the exempt connection (these run before any failpoint is armed).
    function seedDocs(ids) {
        assert.commandWorked(
            exemptConn.getDB(kDb).runCommand({
                insert: kColl,
                documents: ids.map((id) => ({_id: id})),
            }),
        );
    }

    function clearColl() {
        assert.commandWorked(
            exemptConn.getDB(kDb).runCommand({delete: kColl, deletes: [{q: {}, limit: 0}]}),
        );
        assert.commandWorked(
            exemptConn.getDB(kDb).runCommand({delete: kShardKeyColl, deletes: [{q: {}, limit: 0}]}),
        );
    }

    // Count docs matching `filter` directly on a shard primary (bypassing mongos shard-filtering),
    // so an orphan left by a non-atomic shard-key move is visible.
    function countOnShard(rs, filter) {
        let count;
        authutil.asCluster(rs.getPrimary(), kKeyFile, () => {
            count = rs.getPrimary().getDB(kDb)[kShardKeyColl].countDocuments(filter);
        });
        return count;
    }

    // bulkWrite of one insert per _id into `ns` (the only namespace these scenarios touch).
    function bulkInsert(conn, ids, ordered) {
        return conn.getDB("admin").runCommand({
            bulkWrite: 1,
            ops: ids.map((id) => ({insert: 0, document: {_id: id}})),
            nsInfo: [{ns: ns}],
            ordered,
        });
    }

    // updateOne whose filter carries no shard key: mongos cannot target a single shard, so it
    // resolves the op via the two-phase write-without-shard-key protocol
    // (_clusterQueryWithoutShardKey then _clusterWriteWithoutShardKey), run inside an internal
    // transaction. $inc (not $set) makes a double application observable as v == 2.
    function bulkIncOneNoShardKey(conn) {
        return conn.getDB("admin").runCommand({
            bulkWrite: 1,
            ops: [{update: 0, filter: {tag: "wcos"}, updateMods: {$inc: {v: 1}}, multi: false}],
            nsInfo: [{ns: ns}],
            ordered: false,
        });
    }

    function readDoc(id) {
        return exemptConn.getDB(kDb)[kColl].findOne({_id: id});
    }

    // These scenarios only insert, so every summary field other than nErrors/nInserted is 0.
    function assertInsertSummary(res, nErrors, nInserted) {
        summaryFieldsValidator(res, {
            nErrors,
            nInserted,
            nDeleted: 0,
            nMatched: 0,
            nModified: 0,
            nUpserted: 0,
        });
    }

    function assertOpRejected(entry, idx) {
        cursorEntryValidator(entry, {ok: 0, idx, code: k462, n: 0});
    }

    function assertOpInserted(entry, idx) {
        cursorEntryValidator(entry, {ok: 1, idx, n: 1});
    }

    before(function () {
        st = new ShardingTest({
            mongos: 1,
            shards: 2,
            other: {
                auth: "",
                keyFile: kKeyFile,
                // The real IRRL defaults to enabled with huge rate/burst limits; keep it off at
                // startup on every node so only Scenario 1 (mongos) and Scenario 10 (a shard) --
                // the only scenarios that use the real limiter -- configure and enable it, each
                // locally to its own test.
                mongosOptions: {setParameter: {ingressRequestRateLimiterEnabled: false}},
                rsOptions: {setParameter: {ingressRequestRateLimiterEnabled: false}},
            },
        });

        exemptConn = new Mongo(`mongodb://${st.s.host}/?appName=${kRateLimiterExemptAppName}`);
        setupAuth(st.s, exemptConn);

        assert.commandWorked(exemptConn.adminCommand({enableSharding: kDb}));
        ns = `${kDb}.${kColl}`;
        assert.commandWorked(exemptConn.adminCommand({shardCollection: ns, key: {_id: 1}}));
        assert.commandWorked(exemptConn.adminCommand({split: ns, middle: {_id: 0}}));
        // Move both chunks explicitly (the primary shard is non-deterministic) so _id<0 routes to
        // shard0 and _id>=0 routes to shard1.
        assert.commandWorked(
            exemptConn.adminCommand({moveChunk: ns, find: {_id: -1}, to: st.shard0.shardName}),
        );
        assert.commandWorked(
            exemptConn.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard1.shardName}),
        );

        // Second collection sharded on the mutable field `sk`, same chunk layout: sk<0 -> shard0,
        // sk>=0 -> shard1. Used by the shard-key-changing-update scenario.
        skNs = `${kDb}.${kShardKeyColl}`;
        assert.commandWorked(exemptConn.adminCommand({shardCollection: skNs, key: {sk: 1}}));
        assert.commandWorked(exemptConn.adminCommand({split: skNs, middle: {sk: 0}}));
        assert.commandWorked(
            exemptConn.adminCommand({moveChunk: skNs, find: {sk: -1}, to: st.shard0.shardName}),
        );
        assert.commandWorked(
            exemptConn.adminCommand({moveChunk: skNs, find: {sk: 0}, to: st.shard1.shardName}),
        );

        shard0Name = st.shard0.shardName;
        shard1Name = st.shard1.shardName;
        // Read (not set) the startup defaults so afterEach can restore them via setParameter;
        // Scenario 1 relies on these being untouched until a later scenario changes them.
        const rawDefaults = assert.commandWorked(
            exemptConn.adminCommand({
                getParameter: 1,
                defaultClientMaxRetryAttempts: 1,
                defaultClientBaseBackoffMillis: 1,
                defaultClientMaxBackoffMillis: 1,
            }),
        );
        baselineRetryParams = {
            defaultClientMaxRetryAttempts: rawDefaults.defaultClientMaxRetryAttempts,
            defaultClientBaseBackoffMillis: rawDefaults.defaultClientBaseBackoffMillis,
            defaultClientMaxBackoffMillis: rawDefaults.defaultClientMaxBackoffMillis,
        };
    });

    after(function () {
        st.stop();
    });

    afterEach(function () {
        try {
            assert.commandWorked(
                exemptConn.adminCommand({setParameter: 1, ingressRequestRateLimiterEnabled: false}),
            );
        } catch (e) {
            throw new Error(`afterEach: failed to disable IRRL: ${e}`);
        }
        // Clear on every shard node so a stepdown can't leave a stale failpoint on a former primary.
        try {
            for (const node of [...st.rs0.nodes, ...st.rs1.nodes]) {
                disableFailCommandOnShards(node.host);
            }
        } catch (e) {
            throw new Error(`afterEach: failed to clear failCommand on a shard node: ${e}`);
        }
        try {
            setParameter(exemptConn, baselineRetryParams);
        } catch (e) {
            throw new Error(`afterEach: failed to restore overload-retry params: ${e}`);
        }
        clearColl();
    });

    it("Scenario 1: a whole bulkWrite rejected by a real IRRL (budget exhausted) surfaces as a top-level 462 (no per-op cursor) with the overload error labels, and applies no writes", function () {
        // Configure a real (non-injected) IRRL on mongos: kMaxBurstRequests tokens, refilled at an
        // effectively-frozen rate, so a non-exempt client can drain the burst by making
        // kMaxBurstRequests requests before the next one is rejected.
        assert.commandWorked(
            exemptConn.adminCommand({
                configureFailPoint: "ingressRequestRateLimiterFractionalRateOverride",
                mode: "alwaysOn",
                data: {rate: kSlowestRefreshRateSecs},
            }),
        );
        assert.commandWorked(
            exemptConn.adminCommand({
                setParameter: 1,
                ingressRequestAdmissionRatePerSec: 1,
                // bucket depth = refill rate * burstCapacitySecs; with the rate pinned to
                // kSlowestRefreshRateSecs (so it effectively never refills) this is exactly
                // kMaxBurstRequests tokens.
                ingressRequestAdmissionBurstCapacitySecs:
                    kMaxBurstRequests / kSlowestRefreshRateSecs,
                ingressRequestRateLimiterApplicationExemptions: {appNames: kSafeInternalExemptions},
                ingressRequestRateLimiterEnabled: true,
            }),
        );

        const client = new Mongo(`mongodb://${st.s.host}/?appName=nonExemptClient`);
        try {
            authenticateConnection(client);

            // Drain the burst until a request is rejected; the bucket then stays empty.
            assert.soon(() => {
                const r = client.getDB("admin").runCommand({ping: 1});
                return r.ok === 0 && r.code === k462;
            }, "IRRL never rejected a non-exempt main-port request after draining the burst allowance");

            const res = bulkInsert(client, [-1, 1], false);

            assert.commandFailedWithCode(
                res,
                k462,
                "whole bulkWrite must fail at the top level when rejected at the mongos boundary",
            );
            assert(
                !res.hasOwnProperty("cursor"),
                "top-level rejection must not carry a per-op cursor",
                {res},
            );

            // Note these are generated at the mongos boundary, so they should have the retryable
            // error labels.
            assertContainsExpectedErrorLabels(res);
            assert.eq(
                [],
                diskDocs(),
                "no documents must be applied when the whole command is rejected",
            );
        } finally {
            client.close();
            disableRateLimiter(st.s.host);
            assert.commandWorked(
                exemptConn.adminCommand({
                    configureFailPoint: "ingressRequestRateLimiterFractionalRateOverride",
                    mode: "off",
                }),
            );
        }
    });

    it("Scenario 2: UNORDERED bulkWrite with one shard's sub-batch rejected surfaces per-op 462 writeErrors for that shard while the other shard's ops succeed", function () {
        setParameter(exemptConn, {defaultClientMaxRetryAttempts: 0});
        enableOverloadFailCommand(shard0PrimaryHost(), 1);

        // idx 0,2 -> shard0 (_id<0, rejected); idx 1,3 -> shard1 (_id>=0, succeed).
        const statsBefore = getShardStats(exemptConn, shard0Name);
        const res = bulkInsert(exemptConn, [-10, 10, -20, 20], false);
        const statsAfter = getShardStats(exemptConn, shard0Name);

        assert.commandWorked(
            res,
            "unordered bulkWrite reply is top-level ok even with a rejected subset",
        );
        assertInsertSummary(res, 2, 2);
        assertOpRejected(res.cursor.firstBatch[0], 0);
        assertOpInserted(res.cursor.firstBatch[1], 1);
        assertOpRejected(res.cursor.firstBatch[2], 2);
        assertOpInserted(res.cursor.firstBatch[3], 3);

        // idx 0,2 share one shard0 sub-batch: one overload error, no retries (cap 0).
        assertShardingStatisticsDiffEq(shardingStatisticsDifference(statsAfter, statsBefore), {
            numRetriesDueToOverloadAttempted: 0,
            numOverloadErrorsReceived: 1,
        });

        assert.eq([{_id: 10}, {_id: 20}], diskDocs());
    });

    it("Scenario 3: the mongos overload retry strategy retries a rejected shard sub-batch up to the attempt cap, then surfaces the 462 as a per-op writeError", function () {
        const N = 2;
        setParameter(exemptConn, {
            defaultClientMaxRetryAttempts: N,
            defaultClientBaseBackoffMillis: 0,
            defaultClientMaxBackoffMillis: 0,
        });
        // Fail every attempt (initial + N retries) so the cap is genuinely exhausted.
        enableOverloadFailCommand(shard0PrimaryHost(), N + 1);

        const statsBefore = getShardStats(exemptConn, shard0Name);
        const capRes = bulkInsert(exemptConn, [-100, 100], false);
        const statsAfter = getShardStats(exemptConn, shard0Name);

        assert.commandWorked(capRes);
        assertInsertSummary(capRes, 1, 1);
        assertOpRejected(capRes.cursor.firstBatch[0], 0);
        assertOpInserted(capRes.cursor.firstBatch[1], 1);

        // N retries; the overload error is seen on the initial attempt plus each retry (N + 1).
        assertShardingStatisticsDiffEq(shardingStatisticsDifference(statsAfter, statsBefore), {
            numRetriesDueToOverloadAttempted: N,
            numOverloadErrorsReceived: N + 1,
        });
        assert.eq([{_id: 100}], diskDocs(), "only the healthy shard's doc is durable");
    });

    it("Scenario 4: ORDERED bulkWrite stops at the first 462 and does not execute the remainder of the batch", function () {
        setParameter(exemptConn, {defaultClientMaxRetryAttempts: 0});
        enableOverloadFailCommand(shard0PrimaryHost(), 1);

        // idx 0 -> shard1 (succeeds), idx 1 -> shard0 (rejected); idx 2,3 must not execute.
        const res = bulkInsert(exemptConn, [5, -5, 6, -6], true);

        assert.commandWorked(res);
        assertInsertSummary(res, 1, 1);
        // Only the executed prefix is returned (success then first error); idx 2,3 are omitted.
        assert.eq(2, res.cursor.firstBatch.length, "ordered reply stops at the first error", {res});
        assertOpInserted(res.cursor.firstBatch[0], 0);
        assertOpRejected(res.cursor.firstBatch[1], 1);
        assert.eq([{_id: 5}], diskDocs());
    });

    it("Scenario 5: UNORDERED bulkWrite with EVERY shard's sub-batch rejected surfaces a per-op 462 for every op, performs no writes, and records one overload error on each shard", function () {
        setParameter(exemptConn, {defaultClientMaxRetryAttempts: 0});
        // One sub-batch (one `bulkWrite`) is forwarded to each shard, so a single injection per
        // shard rejects the whole fan-out.
        enableOverloadFailCommand(shard0PrimaryHost(), 1);
        enableOverloadFailCommand(shard1PrimaryHost(), 1);

        // idx 0,2 -> shard0 (_id<0); idx 1,3 -> shard1 (_id>=0). All four are rejected.
        const shard0Before = getShardStats(exemptConn, shard0Name);
        const shard1Before = getShardStats(exemptConn, shard1Name);
        const res = bulkInsert(exemptConn, [-1, 1, -2, 2], false);
        const shard0After = getShardStats(exemptConn, shard0Name);
        const shard1After = getShardStats(exemptConn, shard1Name);

        assert.commandWorked(
            res,
            "unordered bulkWrite reply is top-level ok even when every sub-batch is rejected",
        );
        assertInsertSummary(res, 4, 0);
        assertOpRejected(res.cursor.firstBatch[0], 0);
        assertOpRejected(res.cursor.firstBatch[1], 1);
        assertOpRejected(res.cursor.firstBatch[2], 2);
        assertOpRejected(res.cursor.firstBatch[3], 3);

        // No write landed on either shard, so the command as a whole performed no writes.
        assert.eq([], diskDocs(), "no documents must be applied when every sub-batch is rejected");

        // Each shard received one rejected sub-batch; with the cap at 0 there are no retries.
        assertShardingStatisticsDiffEq(shardingStatisticsDifference(shard0After, shard0Before), {
            numRetriesDueToOverloadAttempted: 0,
            numOverloadErrorsReceived: 1,
        });
        assertShardingStatisticsDiffEq(shardingStatisticsDifference(shard1After, shard1Before), {
            numRetriesDueToOverloadAttempted: 0,
            numOverloadErrorsReceived: 1,
        });
    });

    it("Scenario 6: a broadcast multi:true update fanned out to both shards, with only one shard's sub-batch rejected, stays durable on the healthy shard and must NOT advertise NoWritesPerformed", function () {
        setParameter(exemptConn, {defaultClientMaxRetryAttempts: 0});

        // _id<0 -> shard0, _id>=0 -> shard1: two pre-existing matches on each shard.
        seedDocs([-1, -2, 1, 2]);

        // Reject only shard0's sub-batch; shard1 applies its half of the broadcast update.
        enableOverloadFailCommand(shard0PrimaryHost(), 1);

        const shard0Before = getShardStats(exemptConn, shard0Name);
        // {_id: {$lt: 100}} spans both chunk ranges, so this single op fans out to both shards.
        const res = exemptConn.getDB("admin").runCommand({
            bulkWrite: 1,
            ops: [{update: 0, filter: {_id: {$lt: 100}}, updateMods: {$set: {v: 1}}, multi: true}],
            nsInfo: [{ns: ns}],
            ordered: false,
        });
        const shard0After = getShardStats(exemptConn, shard0Name);

        assert.commandWorked(
            res,
            "top-level reply is ok even when one shard's sub-batch of a broadcast op is rejected",
        );

        // The single broadcast op surfaces as a per-op 462 (ok:0) -- but it ALSO reports the
        // healthy shard's applied work (n:2, nModified:2) right on the errored entry. So the op is
        // simultaneously "rejected" and "performed 2 writes"
        cursorEntryValidator(res.cursor.firstBatch[0], {
            ok: 0,
            idx: 0,
            code: k462,
            n: 2,
            nModified: 2,
        });

        assert(
            !res.hasOwnProperty("errorLabels"),
            "an ok:1 bulkWrite reply must not carry top-level errorLabels",
            {res},
        );
        assert(
            !res.cursor.firstBatch[0].hasOwnProperty("errorLabels"),
            "the rejected broadcast op must not carry retry-encouraging labels (NoWritesPerformed/RetryableError)",
            {entry: res.cursor.firstBatch[0]},
        );

        // shard1's docs got `v:1`; shard0's docs are untouched because its sub-batch was rejected.
        assert.eq(
            [{_id: -2}, {_id: -1}, {_id: 1, v: 1}, {_id: 2, v: 1}],
            diskDocsWithV(),
            "only the healthy shard's documents must be modified",
        );

        // mongos saw exactly one overload error (shard0's sub-batch) and did not retry it (cap 0).
        assertShardingStatisticsDiffEq(shardingStatisticsDifference(shard0After, shard0Before), {
            numRetriesDueToOverloadAttempted: 0,
            numOverloadErrorsReceived: 1,
        });
    });

    // Scenarios 7-8 cover updateOne-without-shard-key, which runs inside an internal transaction to
    // drive the two-phase WCOS protocol. A 462 raised in either phase is absorbed by the
    // transaction API's retry loop (independent of the mongos overload-retry strategy, which is
    // pinned to 0 here): the failpoint fires once, the txn API retries, and the op succeeds with
    // the matched doc incremented exactly once.
    it("Scenario 7: updateOne without a shard key -- a 462 on the broadcast query phase (_clusterQueryWithoutShardKey) is retried by the txn API and applies exactly once", function () {
        setParameter(exemptConn, {defaultClientMaxRetryAttempts: 0});
        // Single matching doc on shard0 (_id<0); the filter has no shard key so this is WCOS.
        assert.commandWorked(
            exemptConn.getDB(kDb).runCommand({insert: kColl, documents: [{_id: -5, tag: "wcos"}]}),
        );

        enableFailCommandOnShards(
            shard0PrimaryHost(),
            {times: 1},
            ["_clusterQueryWithoutShardKey"],
            ns,
        );

        const res = bulkIncOneNoShardKey(exemptConn);

        assert.commandWorked(
            res,
            "the txn API must retry the query-phase 462 and the op must succeed",
        );
        cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1, nModified: 1});
        assert.eq(1, readDoc(-5).v, "the matched doc must be incremented exactly once", {res});
    });

    it("Scenario 8: updateOne without a shard key -- a 462 on the targeted write phase (_clusterWriteWithoutShardKey) is retried by the txn API and applies exactly once", function () {
        setParameter(exemptConn, {defaultClientMaxRetryAttempts: 0});
        assert.commandWorked(
            exemptConn.getDB(kDb).runCommand({insert: kColl, documents: [{_id: -5, tag: "wcos"}]}),
        );

        enableFailCommandOnShards(
            shard0PrimaryHost(),
            {times: 1},
            ["_clusterWriteWithoutShardKey"],
            ns,
        );

        const res = bulkIncOneNoShardKey(exemptConn);

        assert.commandWorked(
            res,
            "the txn API must retry the write-phase 462 and the op must succeed",
        );
        cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1, nModified: 1});
        assert.eq(1, readDoc(-5).v, "the matched doc must be incremented exactly once", {res});
    });

    it("Scenario 9: an update that changes a document's shard key value to another shard (WouldChangeOwningShard) leaves the document on exactly one shard under a load-shedding 462", function () {
        setParameter(exemptConn, {defaultClientMaxRetryAttempts: 0});

        // Doc lives on shard0 (sk<0). Setting sk:5 moves it to shard1 (sk>=0), which mongos runs as
        // a distributed transaction: delete the original on shard0, insert the relocated doc on
        // shard1.
        assert.commandWorked(
            exemptConn
                .getDB(kDb)
                .runCommand({insert: kShardKeyColl, documents: [{_id: 100, sk: -5}]}),
        );

        // Reject the move's write on the destination shard once.
        enableFailCommandOnShards(shard1PrimaryHost(), {times: 1}, ["insert"], skNs);

        const res = exemptConn.getDB("admin").runCommand({
            bulkWrite: 1,
            ops: [{update: 0, filter: {sk: -5}, updateMods: {$set: {sk: 5}}, multi: false}],
            nsInfo: [{ns: skNs}],
            ordered: false,
            lsid: {id: UUID()},
            txnNumber: NumberLong(1),
        });

        // The 462 on the insert stage aborts the move's distributed transaction and surfaces as a
        // per-op 462 -- it is not retried because this path does not use the transaction api.
        assert.commandWorked(
            res,
            "top-level reply is ok; the move failure surfaces as a per-op error",
        );
        summaryFieldsValidator(res, {
            nErrors: 1,
            nInserted: 0,
            nMatched: 0,
            nModified: 0,
            nUpserted: 0,
            nDeleted: 0,
        });
        cursorEntryValidator(res.cursor.firstBatch[0], {
            ok: 0,
            idx: 0,
            code: k462,
            n: 0,
            nModified: 0,
        });

        // The doc should be on its original shard only. Query the shards directly so an orphan that
        // mongos shard-filtering would hide is still counted.
        const onShard0 = countOnShard(st.rs0, {_id: 100});
        const onShard1 = countOnShard(st.rs1, {_id: 100});
        assert.eq(1, onShard0, "the original doc must remain on shard0", {res, onShard0, onShard1});
        assert.eq(0, onShard1, "the relocated doc must not have landed on shard1", {
            res,
            onShard0,
            onShard1,
        });
        assert.eq(
            1,
            countOnShard(st.rs0, {_id: 100, sk: -5}),
            "shard0's document must be unchanged (sk still -5)",
        );
    });

    // Canary for Scenarios 2-9's failCommand-injected shape: connects directly to a shard (bypassing
    // mongos) and genuinely exhausts its own real IRRL, so the shard's real admission control
    // produces the rejection. If the shard's real error code/labels ever drifted from what
    // failCommand injects elsewhere in this file, this is the scenario that would catch it.
    it("Scenario 10: a real (non-injected) IRRL rejection on a shard, hit via a normal sharded bulkWrite routed through mongos, surfaces the identical per-op 462 the failCommand-based scenarios above assume", function () {
        setParameter(exemptConn, {defaultClientMaxRetryAttempts: 0});

        const shard0 = st.rs0.getPrimary();
        // Zero burst capacity means the very next non-exempt request reaching shard0 is rejected
        // immediately. exemptConn itself is exempt at the mongos boundary, but mongos forwards
        // each shard's sub-batch under an internal (non-exempt) appName, so the forwarded write to
        // shard0 is genuinely rejected by its own real admission control -- not a failpoint.
        enableZeroBurstRateLimiter(shard0, kSafeInternalExemptions);

        try {
            const statsBefore = getShardStats(exemptConn, shard0Name);
            // idx 0 -> shard0 (_id<0, genuinely rejected); idx 1 -> shard1 (_id>=0, unaffected).
            const res = bulkInsert(exemptConn, [-999, 999], false);
            const statsAfter = getShardStats(exemptConn, shard0Name);

            assert.commandWorked(
                res,
                "unordered bulkWrite reply is top-level ok even when a real (non-injected) shard rejection occurs",
            );
            assertOpRejected(res.cursor.firstBatch[0], 0);
            assertOpInserted(res.cursor.firstBatch[1], 1);

            assertShardingStatisticsDiffEq(shardingStatisticsDifference(statsAfter, statsBefore), {
                numOverloadErrorsReceived: 1,
            });
        } finally {
            disableRateLimiter(shard0.host);
        }
    });
});
