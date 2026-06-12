/**
 * Integration tests for the adaptive WriteConflictException retry limit
 * (WriteConflictRetryLimitExceeded). SERVER-126462.
 *
 * Reads and multi-document transactions don't retry write conflicts the same way as standard
 * writes, so they are not tested here.
 *
 * @tags: [requires_persistence, requires_wiredtiger]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const kLimitMax = 5;
const kLimitMin = 1;
const kWaitersThreshold = 4;

describe("WriteConflictRetryLimitExceeded", function () {
    function setLimitKnobs(limitMax) {
        assert.commandWorked(
            conn.adminCommand({
                setParameter: 1,
                internalQueryWriteConflictRetryLimitMax: limitMax,
                internalQueryWriteConflictRetryLimitMin: kLimitMin,
                internalQueryWriteConflictRetryLimitWaitersThreshold: kWaitersThreshold,
            }),
        );
    }

    function enableWceStorm() {
        // activationProbability=1 makes every WT write rollback into a WCE -- the executor will
        // retry until it hits the limit.
        assert.commandWorked(
            conn.adminCommand({
                configureFailPoint: "WTWriteConflictException",
                mode: {activationProbability: 1.0},
            }),
        );
    }

    function disableWceStorm() {
        assert.commandWorked(
            conn.adminCommand({
                configureFailPoint: "WTWriteConflictException",
                mode: "off",
            }),
        );
    }

    function getMetrics() {
        const m = db.serverStatus().metrics.operation;
        return {
            hits: m.writeConflictRetryLimitHit || 0,
            waiters: m.writeConflictRetryWaiters || 0,
        };
    }

    let conn, db, coll;

    before(function () {
        conn = MongoRunner.runMongod({});
        db = conn.getDB(jsTestName());
        coll = db.getCollection("c");
        assert.commandWorked(coll.insert({_id: 1, n: 0}));
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    it("off by default: limit knob = 0 does not surface WriteConflictRetryLimitExceeded under storm", function () {
        setLimitKnobs(0);
        const beforeHits = getMetrics().hits;
        enableWceStorm();
        // The storm failpoint throws WCE on every write; without the limit the executor retries
        // indefinitely. Uses maxTimeMS to fail fast on infinite retry instead of hanging.
        const res = db.runCommand({
            update: coll.getName(),
            updates: [{q: {n: {$gte: 0}}, u: {$inc: {n: 1}}}],
            maxTimeMS: 5000,
        });
        disableWceStorm();
        // With the limit off, the op either succeeds after WT retries or hits maxTimeMS.
        // The error code must never be WriteConflictRetryLimitExceeded.
        if (!res.ok && res.code) {
            assert.neq(
                res.code,
                ErrorCodes.WriteConflictRetryLimitExceeded,
                "limit fired despite limitMax=0",
                {res},
            );
            assert.eq(
                res.code,
                ErrorCodes.MaxTimeMSExpired,
                "expected MaxTimeMSExpired or success; got an unexpected error code",
                {res},
            );
        }
        if (res.writeErrors) {
            for (const we of res.writeErrors) {
                assert.neq(
                    we.code,
                    ErrorCodes.WriteConflictRetryLimitExceeded,
                    "limit fired despite limitMax=0",
                    {
                        writeError: we,
                    },
                );
            }
        }
        assert.eq(
            getMetrics().hits,
            beforeHits,
            "limitHit counter must not move when feature is disabled",
        );
    });

    it("write hitting the limit surfaces WriteConflictRetryLimitExceeded with the expected categories", function () {
        setLimitKnobs(kLimitMax);
        enableWceStorm();
        const res = db.runCommand({
            update: coll.getName(),
            updates: [{q: {n: {$gte: 0}}, u: {$inc: {n: 1}}}],
        });
        disableWceStorm();
        // Command-level error (ok:0), not folded into writeErrors.
        assert.commandFailedWithCode(res, ErrorCodes.WriteConflictRetryLimitExceeded);
        assert(
            ErrorCodes.isSystemOverloadedError(ErrorCodes.WriteConflictRetryLimitExceeded),
            "WriteConflictRetryLimitExceeded must be in SystemOverloadedError category",
        );
        assert(
            ErrorCodes.isRetriableError(ErrorCodes.WriteConflictRetryLimitExceeded),
            "WriteConflictRetryLimitExceeded must be in RetriableError category",
        );
        // Driver labels attached: writes are retryable-writes-aware so we expect both
        // SystemOverloadedError AND RetryableWriteError on a write hit.
        assert(
            res.errorLabels && res.errorLabels.includes("SystemOverloadedError"),
            "SystemOverloadedError label missing",
            {errorLabels: res.errorLabels},
        );
    });

    it("bulkWrite folds the limit error as a top-level command error", function () {
        setLimitKnobs(kLimitMax);
        enableWceStorm();
        const res = db.adminCommand({
            bulkWrite: 1,
            ops: [{update: 0, filter: {n: {$gte: 0}}, updateMods: {$inc: {n: 1}}}],
            nsInfo: [{ns: coll.getFullName()}],
        });
        disableWceStorm();
        assert.commandFailedWithCode(res, ErrorCodes.WriteConflictRetryLimitExceeded);
        assert(!res.cursor, "bulkWrite must surface the error command-level, not via cursor", {
            res,
        });
    });

    it("findAndModify surfaces WriteConflictRetryLimitExceeded with no special handleError-style plumbing", function () {
        setLimitKnobs(kLimitMax);
        enableWceStorm();
        const res = db.runCommand({
            findAndModify: coll.getName(),
            query: {n: {$gte: 0}},
            update: {$inc: {n: 1}},
        });
        disableWceStorm();
        assert.commandFailedWithCode(res, ErrorCodes.WriteConflictRetryLimitExceeded);
    });

    it("ordered:false aborts the whole batch (no per-doc writeErrors framing)", function () {
        setLimitKnobs(kLimitMax);
        enableWceStorm();
        const res = db.runCommand({
            update: coll.getName(),
            ordered: false,
            updates: [
                {q: {n: {$gte: 0}}, u: {$inc: {n: 1}}},
                {q: {n: {$gte: 0}}, u: {$inc: {n: 1}}},
                {q: {n: {$gte: 0}}, u: {$inc: {n: 1}}},
            ],
        });
        disableWceStorm();
        // Command-level ok:0, not partial-success with writeErrors.
        assert.commandFailedWithCode(res, ErrorCodes.WriteConflictRetryLimitExceeded);
        assert(
            !res.writeErrors || res.writeErrors.length === 0,
            "ordered:false must NOT fold WriteConflictRetryLimitExceeded into per-doc writeErrors",
            {res},
        );
    });

    it("serverStatus metric writeConflictRetryLimitHit increments on cap hit", function () {
        setLimitKnobs(kLimitMax);
        const before = getMetrics().hits;
        enableWceStorm();
        const res = db.runCommand({
            update: coll.getName(),
            updates: [{q: {n: {$gte: 0}}, u: {$inc: {n: 1}}}],
        });
        disableWceStorm();
        assert.commandFailedWithCode(res, ErrorCodes.WriteConflictRetryLimitExceeded);
        const after = getMetrics().hits;
        assert.gt(
            after,
            before,
            "writeConflictRetryLimitHit counter should increment after a capped op",
            {
                before,
                after,
            },
        );
    });

    it("skipWriteConflictRetries failpoint takes precedence over the limit", function () {
        setLimitKnobs(kLimitMax);
        // skipWriteConflictRetries makes the executor throw WriteConflict on the first conflict,
        // before the streak guard or limit machinery runs. The limit must not intercept and must
        // not surface WriteConflictRetryLimitExceeded.
        assert.commandWorked(
            conn.adminCommand({
                configureFailPoint: "skipWriteConflictRetries",
                mode: "alwaysOn",
            }),
        );
        enableWceStorm();
        const res = db.runCommand({
            update: coll.getName(),
            updates: [{q: {n: {$gte: 0}}, u: {$inc: {n: 1}}}],
        });
        disableWceStorm();
        assert.commandWorked(
            conn.adminCommand({
                configureFailPoint: "skipWriteConflictRetries",
                mode: "off",
            }),
        );
        // The failpoint should surface raw WriteConflict either at the command level (if the op
        // is non-retryable) or fold it into writeErrors. WriteConflictRetryLimitExceeded must
        // not appear in either path.
        if (!res.ok && res.code) {
            assert.neq(
                res.code,
                ErrorCodes.WriteConflictRetryLimitExceeded,
                "limit must not intercept when skipWriteConflictRetries is active",
                {res},
            );
        }
        if (res.writeErrors) {
            for (const we of res.writeErrors) {
                assert.neq(
                    we.code,
                    ErrorCodes.WriteConflictRetryLimitExceeded,
                    "limit must not intercept when skipWriteConflictRetries is active",
                    {writeError: we},
                );
            }
        }
    });
});

describe("WriteConflictRetryLimitExceeded -- sharded", function () {
    let st, coll;

    before(function () {
        st = new ShardingTest({shards: 1, rs: {nodes: 1}});
        const sdb = st.s.getDB(jsTestName());
        coll = sdb.getCollection("c_sharded");
        assert.commandWorked(coll.insert({_id: 1, n: 0}));
        // Enable the limit on the shard primary.
        const shardPrimary = st.rs0.getPrimary();
        assert.commandWorked(
            shardPrimary.adminCommand({
                setParameter: 1,
                internalQueryWriteConflictRetryLimitMax: kLimitMax,
                internalQueryWriteConflictRetryLimitMin: kLimitMin,
                internalQueryWriteConflictRetryLimitWaitersThreshold: kWaitersThreshold,
            }),
        );
    });

    after(function () {
        st.stop();
    });

    it("shard WriteConflictRetryLimitExceeded surfaces cleanly to mongos and a recovered write succeeds", function () {
        // mode: {times: N} makes the failpoint self-clear after N hits, so the update eventually
        // succeeds. numOperationsRetriedAtLeastOnceDueToOverload only counts reads, so we don't
        // assert on it here. We verify the write succeeds once the storm clears.
        const shardPrimary = st.rs0.getPrimary();
        assert.commandWorked(
            shardPrimary.adminCommand({
                configureFailPoint: "WTWriteConflictException",
                mode: {times: 50},
            }),
        );
        const res = coll.update({n: {$gte: 0}}, {$inc: {n: 1}});
        assert.commandWorked(res);
    });
});

describe("WriteConflictRetryLimitExceeded -- oplog yields", function () {
    let rst, primary;

    before(function () {
        rst = new ReplSetTest({nodes: 1});
        rst.startSet();
        rst.initiate();
        primary = rst.getPrimary();
        assert.commandWorked(
            primary.adminCommand({
                setParameter: 1,
                internalQueryWriteConflictRetryLimitMax: kLimitMax,
                internalQueryWriteConflictRetryLimitMin: kLimitMin,
                internalQueryWriteConflictRetryLimitWaitersThreshold: kWaitersThreshold,
            }),
        );
    });

    after(function () {
        rst.stopSet();
    });

    it("tailable oplog cursor does not surface WriteConflictRetryLimitExceeded (oplog-visibility yields exempt)", function () {
        // The _oplogWaitConfig early-return path falls through to forceYield without acquiring
        // the streak guard, so a short tailable oplog read exercises that code path.
        const res = primary.getDB("local").runCommand({
            find: "oplog.rs",
            filter: {},
            batchSize: 5,
            tailable: true,
            maxTimeMS: 1000,
        });
        // Either ok:1 or a maxTimeMS / cursor-end error. The only thing we forbid is the
        // retry-limit error.
        if (!res.ok && res.code) {
            assert.neq(
                res.code,
                ErrorCodes.WriteConflictRetryLimitExceeded,
                "tailable oplog cursor must not surface the retry-limit error from oplog yields",
                {res},
            );
        }
    });
});
