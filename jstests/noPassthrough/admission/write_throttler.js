/**
 * Coverage for regular write paths passing through write-throttler admission. Exact batch-aware
 * reconciliation is covered by C++ unit tests because the remaining debit is only exposed through
 * RateLimiter-native state and is timing-sensitive in an end-to-end JS test.
 */

import {after, afterEach, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// kMaxRate mirrors WriteThrottler::kMaxRate (idle / no throttling).
const kMaxRate = 1000 * 1000 * 1000;
// A rate below kMaxRate arms the throttler but is high enough that the test's writes admit
// immediately from burst capacity.
const kArmedRate = 1 * 1000 * 1000;
const kNumDocs = 100;

let rst;
let primary;
let db;
let coll;

describe("write throttler admission", function () {
    before(function () {
        rst = new ReplSetTest({
            nodes: 1,
            nodeOptions: {
                setParameter: {
                    writeThrottlerEnabled: true,
                    // Idle during setup so the initial data load is not throttled.
                    writeThrottlerTargetRatePerSec: kMaxRate,
                    writeThrottlerBurstCapacitySecs: 1.0,
                    writeThrottlerMaxQueueDepth: 1000000,
                    writeThrottlerMaxCostPerOp: 0,
                },
            },
        });
        rst.startSet();
        rst.initiate();

        primary = rst.getPrimary();
        db = primary.getDB(jsTestName());
        coll = db.wt;
    });

    afterEach(function () {
        if (!db || !coll) {
            return;
        }
        disarmThrottler();
        assert.commandWorked(coll.remove({}));
    });

    after(function () {
        if (!rst) {
            return;
        }
        rst.stopSet();
    });

    it("admits batched updates", function () {
        loadDocuments(0, kNumDocs);
        armThrottler();

        const updates = [];
        for (let i = 0; i < kNumDocs; i++) {
            updates.push({q: {_id: i}, u: {$inc: {x: 1}}, multi: false});
        }

        const before = getThrottlerStats();
        const res = assert.commandWorked(db.runCommand({update: coll.getName(), updates}));
        assert.eq(res.n, kNumDocs, res);
        assertThrottlerAdmission(before, getThrottlerStats());
    });

    it("admits insert batches", function () {
        const docs = makeDocuments(0, kNumDocs);
        armThrottler();

        const before = getThrottlerStats();
        assert.commandWorked(coll.insertMany(docs));
        assertThrottlerAdmission(before, getThrottlerStats());
    });

    it("admits batched deletes", function () {
        loadDocuments(0, kNumDocs);
        armThrottler();

        const deletes = [];
        for (let i = 0; i < kNumDocs; i++) {
            deletes.push({q: {_id: i}, limit: 1});
        }

        const before = getThrottlerStats();
        const res = assert.commandWorked(db.runCommand({delete: coll.getName(), deletes}));
        assert.eq(res.n, kNumDocs, res);
        assertThrottlerAdmission(before, getThrottlerStats());
    });

    it("admits findAndModify updates", function () {
        loadDocuments(0, 1);
        armThrottler();

        const before = getThrottlerStats();
        const res = assert.commandWorked(
            db.runCommand({
                findAndModify: coll.getName(),
                query: {_id: 0},
                update: {$inc: {x: 1}},
                new: true,
            }),
        );
        assert.eq(1, res.lastErrorObject.n, res);
        assert.eq(1, res.value.x, res);
        assertThrottlerAdmission(before, getThrottlerStats());
    });

    it("admits findAndModify removes", function () {
        loadDocuments(0, 1);
        armThrottler();

        const before = getThrottlerStats();
        const res = assert.commandWorked(
            db.runCommand({
                findAndModify: coll.getName(),
                query: {_id: 0},
                remove: true,
            }),
        );
        assert.eq(1, res.lastErrorObject.n, res);
        assert.eq(0, res.value._id, res);
        assertThrottlerAdmission(before, getThrottlerStats());
    });
});

function makeDocuments(startId, numDocs) {
    return [...Array(numDocs).keys()].map((i) => ({_id: startId + i, x: 0}));
}

function loadDocuments(startId, numDocs) {
    assert.commandWorked(coll.insert(makeDocuments(startId, numDocs)));
}

function getThrottlerStats() {
    const status = assert.commandWorked(db.adminCommand({serverStatus: 1}));
    assert(status.queues, "expected a queues serverStatus section", {status});
    const wt = status.queues.writeThrottler;
    assert(wt, "expected a queues.writeThrottler serverStatus section", {status});
    return {
        attemptedAdmissions: wt.attemptedAdmissions,
        successfulAdmissions: wt.successfulAdmissions,
        tokensAcquired: wt.tokensAcquired,
    };
}

function armThrottler() {
    assert.commandWorked(
        db.adminCommand({setParameter: 1, writeThrottlerTargetRatePerSec: kArmedRate}),
    );
}

function disarmThrottler() {
    assert.commandWorked(
        db.adminCommand({setParameter: 1, writeThrottlerTargetRatePerSec: kMaxRate}),
    );
}

function assertThrottlerAdmission(before, after) {
    const dAttempts = after.attemptedAdmissions - before.attemptedAdmissions;
    const dAdmissions = after.successfulAdmissions - before.successfulAdmissions;
    const dTokensAcquired = after.tokensAcquired - before.tokensAcquired;

    assert(dAttempts >= 1, "write did not attempt write-throttle admission", {
        before,
        after,
    });
    assert(dAdmissions >= 1, "write did not pass through write-throttle admission", {
        before,
        after,
    });
    assert(dTokensAcquired >= 1, "write did not acquire a write-throttle token", {
        before,
        after,
        dTokensAcquired,
    });
}
