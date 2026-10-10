/**
 * Regression test for a race condition between IncrementalFeatureRolloutContext (IFR) state and
 * getMore retry behavior. We had seen this invariant (SERVER-133148):
 *
 *   Invariant failure: !(*deferred)->hasCachedEgressMetadataForTest()
 *   "Refusing to replace an IFRContext whose egress metadata has already been cached"
 *   at src/mongo/db/ifr_context_decoration.cpp
 *
 * The root cause was the following sequence of events:
 *   1. A getMore's command parsing installs a fresh default IFR context on the new opCtx.
 *   2. Cursor checkout reattaches the AsyncResultsMerger (ARM) to that new opCtx
 *      (cluster_cursor_manager.cpp reattachToOperationContext). This happens BEFORE
 *      cluster_find.cpp's setUpOperationContextStateForGetMore restores the cursor's IFR context
 *      via IncrementalFeatureRolloutContext::set().
 *   3. A delayed ARM retry (scheduled by an earlier, rate-limited remote getMore) fires during that
 *      window.
 *      Prior to the fix, it would re-target its request at the newly attached opCtx and dispatch.
 *      Sending any outgoing network request will cause a router to cache egress metadata on the IFR
 *      context. This later triggers the invariant when the getMore path continues to
 *      setUpOperationContextStateForGetMore.
 *      With the fix, the callback defers — it records backoff and signals the event without
 *      dispatching a network request. The retry is dispatched later by _scheduleGetMores() on the
 *      operation thread, after the IFR context has been restored.
 *
 *   This is challenging to reproduce. To make the scenario deterministic we use two failpoints:
 *   - stallBeforeReDispatchingArmRetry (test-only): stalls the ARM retry callback at its start
 *     (without holding the ARM mutex) so we can line up a *later* getMore's opCtx before the
 *     callback fires.
 *   - waitAfterPinningCursorBeforeGetMoreBatch (existing): holds the victim getMore open exactly in
 *     the window after cursor checkout/reattach but before the IFR set().
 *
 *   The retryable remote failure is produced with failCommand (IngressRequestRateLimitExceeded plus
 *   the SystemOverloadedError/RetryableError labels), scoped to getMore on one shard.
 *
 *   With the fix applied, the ARM retry callback defers instead of re-dispatching. The egress
 *   metadata is never cached on the wrong IFR context, the invariant does not fire, and the
 *   victim getMore completes normally.
 *
 * @tags: [
 *   requires_sharding,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    kExpectedErrorLabels,
    kKeyFile,
    makeKeyfileExemptConn,
    setupAuth,
} from "jstests/noPassthrough/admission/libs/ingress_request_rate_limiter_helper.js";

const kDbName = "test";
const kCollName = "coll";
const kNs = `${kDbName}.${kCollName}`;
const kNumDocsOnShardA = 100;
const kNumDocsOnShardB = 5;

describe("IncrementalFeatureRollout context getMore ARM retry race", function () {
    let st, adminConn, admin, testDB, shardBConn, lsid;

    function runOneGetMore(testDB, cursorId, lsid) {
        return assert.commandWorked(
            testDB.runCommand({
                getMore: cursorId,
                collection: kCollName,
                batchSize: 1,
                lsid,
            }),
        );
    }

    function establishCursor() {
        const findRes = assert.commandWorked(
            testDB.runCommand({find: kCollName, filter: {}, batchSize: 1, lsid}),
        );
        const cursorId = findRes.cursor.id;
        assert.neq(
            0,
            bsonWoCompare({_: cursorId}, {_: NumberLong(0)}),
            "expected an open multi-shard cursor",
            {findRes},
        );
        return cursorId;
    }

    before(function () {
        st = new ShardingTest({
            mongos: 1,
            shards: 2,
            other: {
                keyFile: kKeyFile,
                mongosOptions: {
                    setParameter: {
                        defaultClientMaxRetryAttempts: 1000000,
                    },
                },
            },
        });

        adminConn = new Mongo(st.s.host);
        setupAuth(st.s, adminConn);
        admin = adminConn.getDB("admin");
        testDB = adminConn.getDB(kDbName);

        assert.commandWorked(
            admin.runCommand({enableSharding: kDbName, primaryShard: st.shard0.shardName}),
        );
        assert.commandWorked(admin.runCommand({shardCollection: kNs, key: {_id: 1}}));
        assert.commandWorked(admin.runCommand({split: kNs, middle: {_id: kNumDocsOnShardA}}));
        assert.commandWorked(
            admin.runCommand({
                moveChunk: kNs,
                find: {_id: kNumDocsOnShardA},
                to: st.shard1.shardName,
            }),
        );

        const bulk = testDB[kCollName].initializeUnorderedBulkOp();
        for (let i = 0; i < kNumDocsOnShardA; i++) {
            bulk.insert({_id: i});
        }
        for (let i = kNumDocsOnShardA; i < kNumDocsOnShardA + kNumDocsOnShardB; i++) {
            bulk.insert({_id: i});
        }
        assert.commandWorked(bulk.execute());

        shardBConn = makeKeyfileExemptConn(st.rs1.getPrimary().host);
        lsid = {id: UUID()};
    });

    after(function () {
        st.stop();
    });

    it("does not trigger an IFR invariant when an ARM retry fires during getMore reattach window", function () {
        const bFailCmd = configureFailPoint(shardBConn, "failCommand", {
            errorCode: ErrorCodes.IngressRequestRateLimitExceeded,
            failCommands: ["getMore"],
            failInternalCommands: true,
            namespace: kNs,
            errorLabels: kExpectedErrorLabels,
        });

        const stallRetryFp = configureFailPoint(st.s, "stallBeforeReDispatchingArmRetry");

        const cursorId = establishCursor();

        jsTest.log.info("Draining getMores until the ARM retry to shardB is stalled");
        let stalled = false;
        for (let i = 0; i < 20 && !stalled; i++) {
            runOneGetMore(testDB, cursorId, lsid);
            const res = admin.runCommand({
                waitForFailPoint: "stallBeforeReDispatchingArmRetry",
                timesEntered: stallRetryFp.timesEntered + 1,
                maxTimeMS: 500,
            });
            if (res.ok) {
                stalled = true;
                jsTest.log.info("ARM retry is stalled", {afterDrainIterations: i + 1});
            } else {
                assert.commandFailedWithCode(
                    res,
                    ErrorCodes.MaxTimeMSExpired,
                    "unexpected wait failure",
                );
            }
        }
        assert(
            stalled,
            "the ARM retry to shardB never stalled; the drain loop never triggered a retry",
        );

        const windowFp = configureFailPoint(st.s, "waitAfterPinningCursorBeforeGetMoreBatch");

        jsTest.log.info("Launching victim getMore in a parallel shell");
        const awaitVictim = startParallelShell(
            funWithArgs(
                function (victimCursorId, dbName, collName, victimLsid) {
                    db.getSiblingDB("admin").auth("admin", "pwd");

                    const runGetMore = (cursorId, lsid) => {
                        return db.getSiblingDB(dbName).runCommand({
                            getMore: victimCursorId,
                            collection: collName,
                            batchSize: 1,
                            lsid: victimLsid,
                        });
                    };

                    let res = assert.commandWorked(runGetMore(victimCursorId, victimLsid));
                    jsTest.log.info("victim getMore completed", {res});
                    // That was the crux of it - the first getMore should hit the failpoint and open
                    // the race window. But let's test we can drain the cursor to completion
                    // successfully, not merely complete a single getMore.
                    while (bsonWoCompare({_: res.cursor.id}, {_: NumberLong(0)}) !== 0) {
                        res = assert.commandWorked(runGetMore(victimCursorId, victimLsid));
                        jsTest.log.info("follow-up getMore completed", {res});
                    }
                },
                cursorId,
                kDbName,
                kCollName,
                lsid,
            ),
            st.s.port,
        );

        windowFp.wait();
        jsTest.log.info("Victim getMore is stalled in the post-checkout / pre-IFR-set window");

        bFailCmd.off();
        stallRetryFp.off();
        windowFp.off();

        awaitVictim();

        // Finally, verify that the mongos is still responsive
        assert.commandWorked(admin.runCommand({ping: 1}), "mongos crashed after the race window");
    });
});
