/**
 * Tests that maxTimeMS expires during the mongos backoff sleep between IRRL retries.
 *
 * Three failpoints work together:
 *   - setBackoffDelayForTesting on mongos: deterministic 10s backoff delay between retries
 *   - failCommand on shard (failInternalCommands:true): keeps rejecting throughout backoff window
 *   - maxTimeMS: 3000ms on the find command: fires well inside the 10s window
 *
 * We use failCommand rather than the rate limiter to reject on the shard: it lets us target the
 * find command specifically and inject the overload error at command dispatch, which is all this
 * test needs since we're verifying the mongos backoff behavior, not any shard-side admission.
 *
 * The expected outcome is MaxTimeMSExpired, surfaced via an indirect path: the deadline interrupts
 * the client's own wait for responses, which cancels the pending backoff and produces the error itself,
 * rather than the shard's rejection propagating back directly.
 *
 * @tags: [requires_fcv_80]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    assertShardingStatisticsDiffEq,
    disableFailCommandOnShards,
    enableFailCommandOnShards,
    getShardStats,
    kKeyFile,
    setParameter,
    setupAuth,
    shardingStatisticsDifference,
} from "jstests/noPassthrough/admission/libs/ingress_request_rate_limiter_helper.js";

const kTestNamespace = "test.coll";
const kFindComment = "maxTimeMSBackoffFind";

// Counts operations tagged with `comment` currently running locally on `conn` (mongos or a shard
// mongod). Used to observe whether an in-flight find is being processed on a given node.
function countLocalOpsWithComment(conn, comment) {
    return conn
        .getDB("admin")
        .aggregate([
            {$currentOp: {allUsers: true, localOps: true}},
            {$match: {"command.comment": comment}},
        ])
        .toArray().length;
}

describe("maxTimeMS expiry during IRRL backoff", function () {
    let st;
    let adminMongosConn;

    before(function () {
        st = new ShardingTest({
            mongos: 1,
            shards: 1,
            other: {
                auth: "",
                keyFile: kKeyFile,
            },
        });

        // This test drives retries via failCommand on the shard rather than the rate limiter, so no
        // rate-limiter exemption is needed on the mongos connection.
        adminMongosConn = new Mongo(st.s.host);
        setupAuth(st.s, adminMongosConn);

        assert.commandWorked(
            adminMongosConn.adminCommand({
                enableSharding: "test",
                primaryShard: st.shard0.shardName,
            }),
        );
        assert.commandWorked(
            adminMongosConn.adminCommand({shardCollection: "test.coll", key: {_id: 1}}),
        );
        assert.commandWorked(adminMongosConn.getDB("test").coll.insertOne({_id: 1}));
    });

    after(function () {
        st.stop();
    });

    it("maxTimeMS interrupts backoff sleep", function () {
        const shard0Name = st.shard0.shardName;
        const statsBefore = getShardStats(adminMongosConn, shard0Name);
        const shard0Primary = st.rs0.getPrimary().host;

        // Enable a deterministic 10s backoff on mongos so the opCtx deadline has a clear window
        // to fire before the sleep completes.
        assert.commandWorked(
            adminMongosConn.adminCommand({
                configureFailPoint: "setBackoffDelayForTesting",
                mode: "alwaysOn",
                data: {backoffDelayMs: 10000},
            }),
        );

        // Reject find from mongos to force the ARS retry strategy into the backoff loop.
        enableFailCommandOnShards(shard0Primary, "alwaysOn", ["find"], kTestNamespace);

        const origParams = setParameter(adminMongosConn, {defaultClientMaxRetryAttempts: 3});

        // Run the find in a background thread so we can observe the in-flight operation on mongos
        // while it sits in the retry backoff sleep, and confirm it is cancelled afterwards. A 3s
        // maxTimeMS fires well inside the 10s backoff window while leaving a comfortable window to
        // poll $currentOp before it expires.
        const findThread = new Thread(
            async function (host, comment) {
                const {makeAuthConn} = await import(
                    "jstests/noPassthrough/admission/libs/ingress_request_rate_limiter_helper.js"
                );
                const conn = makeAuthConn(host);
                return conn.getDB("test").runCommand({find: "coll", maxTimeMS: 3000, comment});
            },
            st.s.host,
            kFindComment,
        );

        let findJoined = false;
        try {
            findThread.start();

            // While mongos sleeps in the retry backoff, the find must be visible as an in-flight op
            // on mongos.
            assert.soon(
                () => countLocalOpsWithComment(adminMongosConn, kFindComment) > 0,
                "expected the find to appear as an in-flight op on mongos during the backoff sleep",
            );

            const result = findThread.returnData();
            findJoined = true;

            assert.commandFailedWithCode(
                result,
                ErrorCodes.MaxTimeMSExpired,
                "maxTimeMS should have expired during the 10s backoff window",
            );

            // Once MaxTimeMSExpired surfaces, mongos must have torn down the operation: it no longer
            // appears in $currentOp.
            assert.soon(
                () => countLocalOpsWithComment(adminMongosConn, kFindComment) === 0,
                "expected the find to be cancelled and no longer processed by mongos after MaxTimeMSExpired",
            );

            const diff = shardingStatisticsDifference(
                getShardStats(adminMongosConn, shard0Name),
                statsBefore,
            );
            assertShardingStatisticsDiffEq(diff, {
                numOperationsAttempted: 1,
                numOverloadErrorsReceived: 1,
                // These retry counters are 0 because the retry was aborted during the backoff
                // period before it was dispatched.
                numRetriesDueToOverloadAttempted: 0,
                numOperationsRetriedAtLeastOnceDueToOverload: 0,
                numOperationsRetriedAtLeastOnceDueToOverloadAndSucceeded: 0,
                // recordBackoff() is only called after the sleep completes normally (in the
                // .then() callback after waitUntil). Since the sleep was cancelled by the opCtx
                // deadline, .then() never runs and totalBackoffTimeMillis stays 0. This under-
                // reports the time actually spent in backoff; tracked by SERVER-130458.
                totalBackoffTimeMillis: 0,
            });
        } finally {
            // Ensure the background find is reaped even if an assertion above threw before we
            // joined it.
            if (!findJoined) {
                findThread.join();
            }
            assert.commandWorked(
                adminMongosConn.adminCommand({
                    configureFailPoint: "setBackoffDelayForTesting",
                    mode: "off",
                }),
            );
            disableFailCommandOnShards(shard0Primary);
            setParameter(adminMongosConn, origParams);
        }
    });
});
