/**
 * Tests IRRL error propagation for multi-shard updates and findAndModify.
 *
 * update {multi: true}: non-exempt per-shard executors, so rejections surface as writeErrors
 *   (ok:1), not top-level failures, even when every shard rejects (see SERVER-128710).
 * findAndModify (single-shard targeted): rejections surface as a top-level failure w/ errorLabels.
 * findAndModify with WouldChangeOwningShard: only the internal transaction's commit is exempt
 *   (NetworkInterfaceTL-Sharding-Fixed), so the non-exempt pre-commit delete/insert are covered.
 * findAndModify (can't target a single shard): the two-phase write-without-shard-key protocol
 *   retries inside an internal transaction, independent of defaultClientMaxRetryAttempts, so a
 *   rejection is retried transparently even at 0 client retries.
 *
 * Each case covers budget exhaustion and retry cap; multi-update also covers one vs. both shards.
 *
 * @tags: [requires_fcv_83]
 */

import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
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
// _clusterWriteWithoutShardKey reports its ns() as just the db name (the target collection is
// buried in its opaque inner writeCmd), so failCommand's namespace filter must match on the db
// alone for it, unlike every other command here which reports the full "test.coll" namespace.
const kTestDbName = "test";
const kDisableRetriesParams = {defaultClientMaxRetryAttempts: 0};
const kRetryCapParams = {
    defaultClientMaxRetryAttempts: kRetryAttempts,
    defaultClientBaseBackoffMillis: 0,
    defaultClientMaxBackoffMillis: 0,
};

// Asserts a batch write returned ok:1 with exactly one IngressRequestRateLimitExceeded writeError
// and no top-level errorLabels (see SERVER-128710).
function assertIRRLWriteError(result) {
    assert.eq(result.ok, 1, "expected the batch write to report ok:1 with rate-limit writeErrors", {
        result,
    });
    assert.eq(result.errorLabels, undefined, "batch writes should not propagate errorLabels", {
        result,
    });
    assert.eq(result.writeErrors?.length, 1, "expected one write error", {result});
    assert.eq(
        result.writeErrors[0].code,
        ErrorCodes.IngressRequestRateLimitExceeded,
        "expected IRRL write error",
        {result},
    );
}

describe("multi-shard updates with ingress rate limiter", function () {
    let st;
    let adminMongosConn;

    // Snapshots a shard's stats and arms failCommand on its primary, returning the shard's name,
    // primary host, and pre-test stats for use with shardingStatisticsDifference() afterward.
    function armOverloadFailCommand(shard, replSet, times, command, namespace) {
        const shardName = shard.shardName;
        const statsBefore = getShardStats(adminMongosConn, shardName);
        const primary = replSet.getPrimary().host;
        enableFailCommandOnShards(primary, {times}, [command], namespace);
        return {shardName, primary, statsBefore};
    }

    // Sets `params`, arms failCommand for each entry in `overloads` ({shard, replSet, times,
    // command, namespace}), and runs `fn` with one overload context per entry (see
    // armOverloadFailCommand() for its shape). `namespace` defaults to kTestNamespace and only
    // needs to be overridden for commands, like _clusterWriteWithoutShardKey, that report a
    // db-only ns(). Restores `params` and disables all failCommands afterward, even if `fn`
    // throws.
    function withArmedOverloadFailCommands(params, overloads, fn) {
        const origParams = setParameter(adminMongosConn, params);
        const contexts = overloads.map(
            ({shard, replSet, times, command, namespace = kTestNamespace}) =>
                armOverloadFailCommand(shard, replSet, times, command, namespace),
        );

        try {
            fn(...contexts);
        } finally {
            for (const {primary} of contexts) disableFailCommandOnShards(primary);
            setParameter(adminMongosConn, origParams);
        }
    }

    // Shards test.coll on {x: 1}, splits it at {x: 0}, moves the upper chunk to shard1, and seeds
    // one document per shard.
    function setupShard(adminMongosConn) {
        assert.commandWorked(
            adminMongosConn.adminCommand({
                enableSharding: "test",
                primaryShard: st.shard0.shardName,
            }),
        );
        assert.commandWorked(
            adminMongosConn.adminCommand({shardCollection: "test.coll", key: {x: 1}}),
        );
        assert.commandWorked(adminMongosConn.adminCommand({split: "test.coll", middle: {x: 0}}));
        assert.commandWorked(
            adminMongosConn.adminCommand({
                moveChunk: "test.coll",
                find: {x: 1},
                to: st.shard1.shardName,
            }),
        );
        assert.commandWorked(
            adminMongosConn.getDB("test").coll.insertMany([
                {x: -1, v: 0},
                {x: 1, v: 0},
            ]),
        );
    }

    before(function () {
        st = new ShardingTest({
            mongos: 1,
            shards: 2,
            other: {
                auth: "",
                keyFile: kKeyFile,
                mongosOptions: {
                    // Pins the WouldChangeOwningShard tests below to the legacy (non-transaction-API)
                    // code path, whose pre-commit delete/insert are not exempt from rate limiting.
                    setParameter: {featureFlagUpdateDocumentShardKeyUsingTransactionApi: false},
                },
            },
        });

        // This test drives rejections via failCommand on the shards rather than the ingress rate
        // limiter, so no rate-limiter exemption is needed on the mongos connection.
        adminMongosConn = new Mongo(st.s.host);
        setupAuth(st.s, adminMongosConn);

        setupShard(adminMongosConn);
    });

    after(function () {
        st.stop();
    });

    describe("update {multi: true}", function () {
        it("budget exhaustion: multi-update fails immediately with retries disabled", function () {
            withArmedOverloadFailCommands(
                kDisableRetriesParams,
                [{shard: st.shard0, replSet: st.rs0, times: 1, command: "update"}],
                ({shardName, statsBefore}) => {
                    const result = adminMongosConn.getDB("test").runCommand({
                        update: "coll",
                        updates: [{q: {}, u: {$inc: {v: 1}}, multi: true}],
                    });
                    assertIRRLWriteError(result);

                    const diff = shardingStatisticsDifference(
                        getShardStats(adminMongosConn, shardName),
                        statsBefore,
                    );
                    assertShardingStatisticsDiffEq(diff, {
                        numOperationsAttempted: 1,
                        numOverloadErrorsReceived: 1,
                        numRetriesDueToOverloadAttempted: 0,
                        numOperationsRetriedAtLeastOnceDueToOverload: 0,
                        numOperationsRetriedAtLeastOnceDueToOverloadAndSucceeded: 0,
                        totalBackoffTimeMillis: 0,
                    });
                },
            );
        });

        it("retry cap: multi-update fails after exhausting all retries", function () {
            withArmedOverloadFailCommands(
                kRetryCapParams,
                [{shard: st.shard0, replSet: st.rs0, times: kRetryAttempts + 1, command: "update"}],
                ({shardName, statsBefore}) => {
                    const result = adminMongosConn.getDB("test").runCommand({
                        update: "coll",
                        updates: [{q: {}, u: {$inc: {v: 1}}, multi: true}],
                    });
                    assertIRRLWriteError(result);

                    const diff = shardingStatisticsDifference(
                        getShardStats(adminMongosConn, shardName),
                        statsBefore,
                    );
                    assertShardingStatisticsDiffEq(diff, {
                        numOperationsAttempted: 1,
                        numOverloadErrorsReceived: kRetryAttempts + 1,
                        numRetriesDueToOverloadAttempted: kRetryAttempts,
                        numOperationsRetriedAtLeastOnceDueToOverload: 1,
                        numOperationsRetriedAtLeastOnceDueToOverloadAndSucceeded: 0,
                        totalBackoffTimeMillis: 0,
                    });
                },
            );
        });

        it("budget exhaustion: multi-update fails on both shards with retries disabled", function () {
            withArmedOverloadFailCommands(
                kDisableRetriesParams,
                [
                    {shard: st.shard0, replSet: st.rs0, times: 1, command: "update"},
                    {shard: st.shard1, replSet: st.rs1, times: 1, command: "update"},
                ],
                (shard0, shard1) => {
                    const result = adminMongosConn.getDB("test").runCommand({
                        update: "coll",
                        updates: [{q: {}, u: {$inc: {v: 1}}, multi: true}],
                    });
                    assertIRRLWriteError(result);

                    // A single multi: true statement that fans out to two shards and fails on
                    // both still only produces one write error.
                    for (const {shardName, statsBefore} of [shard0, shard1]) {
                        const diff = shardingStatisticsDifference(
                            getShardStats(adminMongosConn, shardName),
                            statsBefore,
                        );
                        assertShardingStatisticsDiffEq(diff, {
                            numOperationsAttempted: 1,
                            numOverloadErrorsReceived: 1,
                            numRetriesDueToOverloadAttempted: 0,
                            numOperationsRetriedAtLeastOnceDueToOverload: 0,
                            numOperationsRetriedAtLeastOnceDueToOverloadAndSucceeded: 0,
                            totalBackoffTimeMillis: 0,
                        });
                    }
                },
            );
        });

        it("retry cap: multi-update fails on both shards after exhausting all retries", function () {
            withArmedOverloadFailCommands(
                kRetryCapParams,
                [
                    {
                        shard: st.shard0,
                        replSet: st.rs0,
                        times: kRetryAttempts + 1,
                        command: "update",
                    },
                    {
                        shard: st.shard1,
                        replSet: st.rs1,
                        times: kRetryAttempts + 1,
                        command: "update",
                    },
                ],
                (shard0, shard1) => {
                    const result = adminMongosConn.getDB("test").runCommand({
                        update: "coll",
                        updates: [{q: {}, u: {$inc: {v: 1}}, multi: true}],
                    });
                    assertIRRLWriteError(result);

                    for (const {shardName, statsBefore} of [shard0, shard1]) {
                        const diff = shardingStatisticsDifference(
                            getShardStats(adminMongosConn, shardName),
                            statsBefore,
                        );
                        assertShardingStatisticsDiffEq(diff, {
                            numOperationsAttempted: 1,
                            numOverloadErrorsReceived: kRetryAttempts + 1,
                            numRetriesDueToOverloadAttempted: kRetryAttempts,
                            numOperationsRetriedAtLeastOnceDueToOverload: 1,
                            numOperationsRetriedAtLeastOnceDueToOverloadAndSucceeded: 0,
                            totalBackoffTimeMillis: 0,
                        });
                    }
                },
            );
        });
    });

    describe("findAndModify", function () {
        it("budget exhaustion: findAndModify fails immediately with retries disabled", function () {
            withArmedOverloadFailCommands(
                kDisableRetriesParams,
                [{shard: st.shard0, replSet: st.rs0, times: 1, command: "findAndModify"}],
                ({shardName, statsBefore}) => {
                    const result = adminMongosConn.getDB("test").runCommand({
                        findAndModify: "coll",
                        // Targets shard0 exclusively.
                        query: {x: -1},
                        update: {$inc: {v: 1}},
                    });
                    assertMongosIRRLCommandFailure(
                        result,
                        "findAndModify must fail with shard rate limit rejection",
                    );

                    const diff = shardingStatisticsDifference(
                        getShardStats(adminMongosConn, shardName),
                        statsBefore,
                    );
                    assertShardingStatisticsDiffEq(diff, {
                        numOperationsAttempted: 1,
                        numOverloadErrorsReceived: 1,
                        numRetriesDueToOverloadAttempted: 0,
                        numOperationsRetriedAtLeastOnceDueToOverload: 0,
                        numOperationsRetriedAtLeastOnceDueToOverloadAndSucceeded: 0,
                        totalBackoffTimeMillis: 0,
                    });
                },
            );
        });

        it("retry cap: findAndModify fails after exhausting all retries", function () {
            withArmedOverloadFailCommands(
                kRetryCapParams,
                [
                    {
                        shard: st.shard0,
                        replSet: st.rs0,
                        times: kRetryAttempts + 1,
                        command: "findAndModify",
                    },
                ],
                ({shardName, statsBefore}) => {
                    const result = adminMongosConn.getDB("test").runCommand({
                        findAndModify: "coll",
                        query: {x: -1},
                        update: {$inc: {v: 1}},
                    });
                    assertMongosIRRLCommandFailure(
                        result,
                        "findAndModify must fail after exhausting all retries",
                    );

                    const diff = shardingStatisticsDifference(
                        getShardStats(adminMongosConn, shardName),
                        statsBefore,
                    );
                    assertShardingStatisticsDiffEq(diff, {
                        numOperationsAttempted: 1,
                        numOverloadErrorsReceived: kRetryAttempts + 1,
                        numRetriesDueToOverloadAttempted: kRetryAttempts,
                        numOperationsRetriedAtLeastOnceDueToOverload: 1,
                        numOperationsRetriedAtLeastOnceDueToOverloadAndSucceeded: 0,
                        totalBackoffTimeMillis: 0,
                    });
                },
            );
        });

        it("succeeds despite an IRRL rejection when the query can't target a single shard, because the internal transaction API retries independently of the client budget", function () {
            // {tag: "no-shard-key"} omits the shard key (x), so mongos can't target a single
            // shard and instead runs the two-phase write-without-shard-key protocol.
            const kNoShardKeyId = "no-shard-key-target";
            assert.commandWorked(
                adminMongosConn
                    .getDB("test")
                    .coll.insertOne({_id: kNoShardKeyId, x: -1, tag: "no-shard-key", v: 0}),
            );

            function runFindAndModify() {
                const result = adminMongosConn.getDB("test").runCommand({
                    findAndModify: "coll",
                    query: {tag: "no-shard-key"},
                    update: {$inc: {v: 1}},
                });
                assert.commandWorked(
                    result,
                    "the internal transaction API must retry the write-phase rejection and " +
                        "the command must succeed",
                );
                assert.eq(result.lastErrorObject.n, 1, "expected exactly one document modified", {
                    result,
                });

                const doc = adminMongosConn.getDB("test").coll.findOne({_id: kNoShardKeyId});
                assert.eq(doc.v, 1, "expected the matched doc to be incremented exactly once", {
                    doc,
                });
            }

            try {
                withArmedOverloadFailCommands(
                    kDisableRetriesParams,
                    [
                        {
                            shard: st.shard0,
                            replSet: st.rs0,
                            times: 1,
                            command: "_clusterWriteWithoutShardKey",
                            namespace: kTestDbName,
                        },
                    ],
                    runFindAndModify,
                );
            } finally {
                assert.commandWorked(
                    adminMongosConn.getDB("test").coll.deleteOne({_id: kNoShardKeyId}),
                );
            }
        });
    });

    describe("findAndModify with WouldChangeOwningShard", function () {
        const kWcosId = "wcos-doc";
        const kWcosOriginX = -5;
        const kWcosDestinationX = 5;

        // WouldChangeOwningShard requires a retryable write (or an explicit transaction) to be
        // handled by the legacy path; this connection has no implicit session, so lsid/txnNumber
        // are attached explicitly.
        function runWcosFindAndModify() {
            return adminMongosConn.getDB("test").runCommand({
                findAndModify: "coll",
                query: {_id: kWcosId, x: kWcosOriginX},
                update: {$set: {x: kWcosDestinationX}},
                lsid: {id: UUID()},
                txnNumber: NumberLong(0),
            });
        }

        beforeEach(function () {
            // {x: -5} targets shard0 exclusively; the update above moves it into shard1's range.
            assert.commandWorked(
                adminMongosConn.getDB("test").coll.insertOne({_id: kWcosId, x: kWcosOriginX, v: 0}),
            );
        });

        afterEach(function () {
            // The internal transaction is aborted on failure, so the doc is always still at its
            // original location; broadcast delete by _id works regardless.
            assert.commandWorked(adminMongosConn.getDB("test").coll.deleteMany({_id: kWcosId}));
        });

        // numOperationsAttempted counts one per versioned command dispatched to the shard
        // (regardless of retries), not just the delete/insert step itself. shard0 also gets the
        // original findAndModify and its in-transaction rerun (both hitting WouldChangeOwningShard),
        // plus the final abortTransaction: 2 findAndModify + 1 delete + 1 abort = 4. shard1 only gets
        // the insert plus the abortTransaction: 1 insert + 1 abort = 2.
        const kExpectedShard0OperationsAttempted = 4;
        const kExpectedShard1OperationsAttempted = 2;

        it("budget exhaustion: delete step fails immediately with retries disabled", function () {
            withArmedOverloadFailCommands(
                kDisableRetriesParams,
                [{shard: st.shard0, replSet: st.rs0, times: 1, command: "delete"}],
                ({shardName, statsBefore}) => {
                    const result = runWcosFindAndModify();
                    assertMongosIRRLCommandFailure(
                        result,
                        "WCOS findAndModify must fail when the pre-commit delete is rate limited",
                    );

                    const diff = shardingStatisticsDifference(
                        getShardStats(adminMongosConn, shardName),
                        statsBefore,
                    );
                    assertShardingStatisticsDiffEq(diff, {
                        numOperationsAttempted: kExpectedShard0OperationsAttempted,
                        numOverloadErrorsReceived: 1,
                        numRetriesDueToOverloadAttempted: 0,
                    });
                },
            );
        });

        it("retry cap: delete step fails after exhausting all retries", function () {
            withArmedOverloadFailCommands(
                kRetryCapParams,
                [{shard: st.shard0, replSet: st.rs0, times: kRetryAttempts + 1, command: "delete"}],
                ({shardName, statsBefore}) => {
                    const result = runWcosFindAndModify();
                    assertMongosIRRLCommandFailure(
                        result,
                        "WCOS findAndModify must fail after exhausting all retries on the delete step",
                    );

                    const diff = shardingStatisticsDifference(
                        getShardStats(adminMongosConn, shardName),
                        statsBefore,
                    );
                    assertShardingStatisticsDiffEq(diff, {
                        numOperationsAttempted: kExpectedShard0OperationsAttempted,
                        numOverloadErrorsReceived: kRetryAttempts + 1,
                        numRetriesDueToOverloadAttempted: kRetryAttempts,
                    });
                },
            );
        });

        it("budget exhaustion: insert step fails immediately with retries disabled", function () {
            withArmedOverloadFailCommands(
                kDisableRetriesParams,
                [{shard: st.shard1, replSet: st.rs1, times: 1, command: "insert"}],
                ({shardName, statsBefore}) => {
                    const result = runWcosFindAndModify();
                    assertMongosIRRLCommandFailure(
                        result,
                        "WCOS findAndModify must fail when the pre-commit insert is rate limited",
                    );

                    const diff = shardingStatisticsDifference(
                        getShardStats(adminMongosConn, shardName),
                        statsBefore,
                    );
                    assertShardingStatisticsDiffEq(diff, {
                        numOperationsAttempted: kExpectedShard1OperationsAttempted,
                        numOverloadErrorsReceived: 1,
                        numRetriesDueToOverloadAttempted: 0,
                    });
                },
            );
        });

        it("retry cap: insert step fails after exhausting all retries", function () {
            withArmedOverloadFailCommands(
                kRetryCapParams,
                [{shard: st.shard1, replSet: st.rs1, times: kRetryAttempts + 1, command: "insert"}],
                ({shardName, statsBefore}) => {
                    const result = runWcosFindAndModify();
                    assertMongosIRRLCommandFailure(
                        result,
                        "WCOS findAndModify must fail after exhausting all retries on the insert step",
                    );

                    const diff = shardingStatisticsDifference(
                        getShardStats(adminMongosConn, shardName),
                        statsBefore,
                    );
                    assertShardingStatisticsDiffEq(diff, {
                        numOperationsAttempted: kExpectedShard1OperationsAttempted,
                        numOverloadErrorsReceived: kRetryAttempts + 1,
                        numRetriesDueToOverloadAttempted: kRetryAttempts,
                    });
                },
            );
        });
    });
});
