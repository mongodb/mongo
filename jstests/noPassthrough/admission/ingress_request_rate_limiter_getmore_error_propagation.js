/**
 * Tests IRRL error propagation for getMore on a multi-batch cursor. Mongos-to-shard getMore calls
 * route through NetworkInterfaceTL-TaskExecutorPool-{i} (non-exempt), so shard rejections
 * propagate back through mongos. The cursor is opened before the failpoint is armed so the
 * initial find succeeds; only the subsequent getMore is rejected.
 *
 * Covered on both a single-shard cluster and a two-shard cluster:
 *   - Budget exhaustion: 0 retries, failpoint fires once → getMore fails immediately.
 *   - Retry cap: K retries, failpoint fires K+1 times → getMore fails after exhausting retries.
 *
 * The two-shard cluster additionally exercises the error-merging logic in the AsyncResultsMerger
 * by fanning the getMore out to both shards and rejecting on one shard vs. both shards.
 *
 * @tags: [requires_fcv_80]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    assertMongosIRRLCommandFailure,
    assertShardingStatisticsDiffEq,
    disableFailCommandOnShards,
    enableFailCommandOnShards,
    getShardStats,
    kKeyFile,
    setParameter,
    setupAuth,
    shardingStatisticsDifference,
} from "jstests/noPassthrough/admission/libs/ingress_request_rate_limiter_helper.js";

const kRetryAttempts = 3;
const kTestNamespace = "test.coll";
const kDisableRetriesParams = {defaultClientMaxRetryAttempts: 0};
const kRetryCapParams = {
    defaultClientMaxRetryAttempts: kRetryAttempts,
    defaultClientBaseBackoffMillis: 0,
    defaultClientMaxBackoffMillis: 0,
};

/**
 * Returns a map of shard name -> number of idle cursors open on that shard for the test namespace,
 * gathered in a single $currentOp through mongos. Used to confirm that the initial find really left
 * a live cursor on each shard the getMore is expected to revisit.
 */
function openShardCursorCounts(adminConn) {
    const res = assert.commandWorked(
        adminConn.getDB("admin").runCommand({
            aggregate: 1,
            pipeline: [
                {$currentOp: {idleCursors: true, allUsers: true}},
                {$match: {type: "idleCursor", ns: kTestNamespace}},
                {$group: {_id: "$shard", count: {$sum: 1}}},
            ],
            cursor: {},
        }),
    );
    const counts = {};
    for (const doc of res.cursor.firstBatch) {
        counts[doc._id] = doc.count;
    }
    return counts;
}

/**
 * Asserts the per-shard overload counters for a getMore that both shards reject. The remotes race,
 * so the error split is nondeterministic: at least one shard hits `cap` (the failpoint `times`) and
 * each sees between 0 and `cap` errors. A retry is only counted once it actually executes, so the
 * terminal error never gets a following attempt: a shard that saw N errors retried max(N - 1, 0).
 */
function assertRacedOverloadDiffs(shard0Diff, shard1Diff, {cap}) {
    for (const diff of [shard0Diff, shard1Diff]) {
        assert.between(0, diff.numOverloadErrorsReceived, cap, {diff});
        assert.eq(
            diff.numRetriesDueToOverloadAttempted,
            Math.max(diff.numOverloadErrorsReceived - 1, 0),
            "unexpected numRetriesDueToOverloadAttempted",
            {diff},
        );
    }
    assert.eq(
        Math.max(shard0Diff.numOverloadErrorsReceived, shard1Diff.numOverloadErrorsReceived),
        cap,
        "at least one shard must hit the full error cap",
        {shard0Diff, shard1Diff},
    );
}

/**
 * Runs the common getMore-rejection flow against a cluster:
 *   1. Applies `params`, snapshotting the originals to restore afterwards.
 *   2. Opens a partial-batch cursor and asserts every shard in `shards` still has a live cursor,
 *      so the getMore genuinely fans out to (and is rejected on) the shards under test.
 *   3. Arms the getMore failCommand failpoint with `mode` on every shard flagged `arm: true`.
 *   4. Runs the getMore, asserts the IRRL failure, and invokes `checkStats(diffsByShardName)` with
 *      the per-shard shardingStatistics deltas.
 * Failpoints are always disabled, the cursor killed, and parameters restored.
 *
 * `shards` is an array of {name, host, arm}. `adminConn` is used for stats and cursor cleanup;
 * `dataConn` issues the find/getMore. Rejections come from failCommand on the shards, not the
 * ingress rate limiter, so neither connection needs a rate-limiter exemption.
 */
function runGetMoreRejection({adminConn, dataConn, params, mode, shards, checkStats}) {
    const origParams = setParameter(adminConn, params);
    const statsBefore = {};
    for (const s of shards) {
        statsBefore[s.name] = getShardStats(adminConn, s.name);
    }

    const findResult = assert.commandWorked(
        dataConn.getDB("test").runCommand({find: "coll", batchSize: 2}),
    );
    const cursorId = findResult.cursor.id;
    assert.neq(cursorId, 0, "cursor must remain open after partial batch", {findResult});
    const cursorCounts = openShardCursorCounts(adminConn);
    for (const s of shards) {
        assert.gte(
            cursorCounts[s.name] ?? 0,
            1,
            `shard ${s.name} must have an open cursor after the initial find so the getMore revisits it`,
            {cursorCounts},
        );
    }

    const armed = shards.filter((s) => s.arm);
    for (const s of armed) {
        enableFailCommandOnShards(s.host, mode, ["getMore"], kTestNamespace);
    }
    try {
        const result = dataConn.getDB("test").runCommand({getMore: cursorId, collection: "coll"});
        assertMongosIRRLCommandFailure(result, "getMore must fail with shard rate limit rejection");

        const diffs = {};
        for (const s of shards) {
            diffs[s.name] = shardingStatisticsDifference(
                getShardStats(adminConn, s.name),
                statsBefore[s.name],
            );
        }
        checkStats(diffs);
    } finally {
        for (const s of armed) {
            disableFailCommandOnShards(s.host);
        }
        // A failed getMore does not auto-close the mongos-side cursor, so must explicitly kill it.
        adminConn.getDB("test").runCommand({killCursors: "coll", cursors: [cursorId]});
        setParameter(adminConn, origParams);
    }
}

describe("getMore with ingress rate limiter on a single-shard cluster", function () {
    let st;
    let adminMongosConn;

    // Shards test.coll on {_id: 1} and seeds five documents.
    function setupOneShard(adminMongosConn) {
        assert.commandWorked(
            adminMongosConn.adminCommand({
                enableSharding: "test",
                primaryShard: st.shard0.shardName,
            }),
        );
        assert.commandWorked(
            adminMongosConn.adminCommand({shardCollection: "test.coll", key: {_id: 1}}),
        );
        assert.commandWorked(
            adminMongosConn
                .getDB("test")
                .coll.insertMany([{_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}, {_id: 5}]),
        );
    }

    before(function () {
        st = new ShardingTest({
            mongos: 1,
            shards: 1,
            other: {
                auth: "",
                keyFile: kKeyFile,
            },
        });

        // This test drives rejections via failCommand on the shards rather than the ingress rate
        // limiter, so no rate-limiter exemption is needed on the mongos connection.
        adminMongosConn = new Mongo(st.s.host);
        setupAuth(st.s, adminMongosConn);

        setupOneShard(adminMongosConn);
    });

    after(function () {
        st.stop();
    });

    it("budget exhaustion: getMore fails immediately with retries disabled", function () {
        const shardName = st.shard0.shardName;
        runGetMoreRejection({
            adminConn: adminMongosConn,
            dataConn: adminMongosConn,
            params: kDisableRetriesParams,
            mode: {times: 1},
            shards: [{name: shardName, host: st.rs0.getPrimary().host, arm: true}],
            checkStats: (diffs) =>
                assertShardingStatisticsDiffEq(diffs[shardName], {
                    numOverloadErrorsReceived: 1,
                    numRetriesDueToOverloadAttempted: 0,
                    numOperationsRetriedAtLeastOnceDueToOverload: 0,
                    numOperationsRetriedAtLeastOnceDueToOverloadAndSucceeded: 0,
                    totalBackoffTimeMillis: 0,
                }),
        });
    });

    it("retry cap: getMore fails after exhausting all retries", function () {
        const shardName = st.shard0.shardName;
        runGetMoreRejection({
            adminConn: adminMongosConn,
            dataConn: adminMongosConn,
            params: kRetryCapParams,
            mode: {times: kRetryAttempts + 1},
            shards: [{name: shardName, host: st.rs0.getPrimary().host, arm: true}],
            checkStats: (diffs) =>
                assertShardingStatisticsDiffEq(diffs[shardName], {
                    numOverloadErrorsReceived: kRetryAttempts + 1,
                    numRetriesDueToOverloadAttempted: kRetryAttempts,
                    numOperationsRetriedAtLeastOnceDueToOverload: 1,
                    numOperationsRetriedAtLeastOnceDueToOverloadAndSucceeded: 0,
                    totalBackoffTimeMillis: 0,
                }),
        });
    });
});

describe("getMore with ingress rate limiter on a two-shard cluster", function () {
    let st;
    let adminMongosConn;

    // Shards test.coll on {_id: 1}, splits it at {_id: 0}, moves the upper chunk to shard1, and
    // seeds three documents per shard.
    function setupTwoShards(adminMongosConn) {
        assert.commandWorked(
            adminMongosConn.adminCommand({
                enableSharding: "test",
                primaryShard: st.shard0.shardName,
            }),
        );
        assert.commandWorked(
            adminMongosConn.adminCommand({shardCollection: "test.coll", key: {_id: 1}}),
        );
        assert.commandWorked(adminMongosConn.adminCommand({split: "test.coll", middle: {_id: 0}}));
        assert.commandWorked(
            adminMongosConn.adminCommand({
                moveChunk: "test.coll",
                find: {_id: 1},
                to: st.shard1.shardName,
            }),
        );
        // Enough docs on each side that a small initial batch leaves both shard cursors open, so
        // the subsequent getMore fans out to both shards.
        assert.commandWorked(
            adminMongosConn
                .getDB("test")
                .coll.insertMany([{_id: -3}, {_id: -2}, {_id: -1}, {_id: 1}, {_id: 2}, {_id: 3}]),
        );
    }

    before(function () {
        st = new ShardingTest({
            mongos: 1,
            shards: 2,
            other: {
                auth: "",
                keyFile: kKeyFile,
            },
        });

        // This test drives rejections via failCommand on the shards rather than the ingress rate
        // limiter, so no rate-limiter exemption is needed on the mongos connection.
        adminMongosConn = new Mongo(st.s.host);
        setupAuth(st.s, adminMongosConn);

        setupTwoShards(adminMongosConn);
    });

    after(function () {
        st.stop();
    });

    describe("one shard rejects", function () {
        // Only shard1's getMore is rejected; shard0's getMore succeeds. First-non-OK-wins in the
        // AsyncResultsMerger must still fail the whole getMore and discard shard0's buffered docs.
        function shardSpecs() {
            return [
                {name: st.shard0.shardName, host: st.rs0.getPrimary().host, arm: false},
                {name: st.shard1.shardName, host: st.rs1.getPrimary().host, arm: true},
            ];
        }

        it("budget exhaustion: getMore fails immediately with retries disabled", function () {
            const shard0 = st.shard0.shardName;
            const shard1 = st.shard1.shardName;
            runGetMoreRejection({
                adminConn: adminMongosConn,
                dataConn: adminMongosConn,
                params: kDisableRetriesParams,
                mode: {times: 1},
                shards: shardSpecs(),
                checkStats: (diffs) => {
                    assertShardingStatisticsDiffEq(diffs[shard1], {
                        numOverloadErrorsReceived: 1,
                        numRetriesDueToOverloadAttempted: 0,
                        numOperationsRetriedAtLeastOnceDueToOverload: 0,
                        numOperationsRetriedAtLeastOnceDueToOverloadAndSucceeded: 0,
                        totalBackoffTimeMillis: 0,
                    });
                    // shard0's getMore succeeded, so it saw no overload errors.
                    assertShardingStatisticsDiffEq(diffs[shard0], {numOverloadErrorsReceived: 0});
                },
            });
        });

        it("retry cap: getMore fails after exhausting all retries on one shard", function () {
            const shard0 = st.shard0.shardName;
            const shard1 = st.shard1.shardName;
            runGetMoreRejection({
                adminConn: adminMongosConn,
                dataConn: adminMongosConn,
                params: kRetryCapParams,
                mode: {times: kRetryAttempts + 1},
                shards: shardSpecs(),
                checkStats: (diffs) => {
                    assertShardingStatisticsDiffEq(diffs[shard1], {
                        numOverloadErrorsReceived: kRetryAttempts + 1,
                        numRetriesDueToOverloadAttempted: kRetryAttempts,
                        numOperationsRetriedAtLeastOnceDueToOverload: 1,
                        numOperationsRetriedAtLeastOnceDueToOverloadAndSucceeded: 0,
                        totalBackoffTimeMillis: 0,
                    });
                    assertShardingStatisticsDiffEq(diffs[shard0], {numOverloadErrorsReceived: 0});
                },
            });
        });
    });

    describe("both shards reject", function () {
        function shardSpecs() {
            return [
                {name: st.shard0.shardName, host: st.rs0.getPrimary().host, arm: true},
                {name: st.shard1.shardName, host: st.rs1.getPrimary().host, arm: true},
            ];
        }

        it("budget exhaustion: getMore fails immediately with retries disabled", function () {
            const shard0 = st.shard0.shardName;
            const shard1 = st.shard1.shardName;
            runGetMoreRejection({
                adminConn: adminMongosConn,
                dataConn: adminMongosConn,
                params: kDisableRetriesParams,
                mode: {times: 1},
                shards: shardSpecs(),
                checkStats: (diffs) =>
                    assertRacedOverloadDiffs(diffs[shard0], diffs[shard1], {cap: 1}),
            });
        });

        it("retry cap: getMore fails after exhausting all retries on both shards", function () {
            const shard0 = st.shard0.shardName;
            const shard1 = st.shard1.shardName;
            runGetMoreRejection({
                adminConn: adminMongosConn,
                dataConn: adminMongosConn,
                params: kRetryCapParams,
                mode: {times: kRetryAttempts + 1},
                shards: shardSpecs(),
                checkStats: (diffs) =>
                    assertRacedOverloadDiffs(diffs[shard0], diffs[shard1], {
                        cap: kRetryAttempts + 1,
                    }),
            });
        });
    });
});
