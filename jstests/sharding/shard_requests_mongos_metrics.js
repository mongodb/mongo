/**
 * Verifies that mongos shard request metrics in serverStatus behave correctly. The three
 * counters (inQueue, awaitingResponse, complete) flow in order; the three failpoints act
 * as gates between them:
 *
 * - blockShardRequestBeforeQueueing: before inQueue (1st counter)
 * - blockShardRequestBeforeSending: between inQueue and awaitingResponse (2nd counter)
 * - blockShardRequestBeforeResolving: between awaitingResponse and complete (3rd counter)
 *
 * One sequence runs through all three gates to assert the metrics and failpoints behave
 * as expected. There may be additional requests from other activity, so we use inequality
 * checks where appropriate.
 *
 * @tags: [
 *   requires_sharding,
 *   assumes_balancer_off,
 *   multiversion_incompatible,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

function coerceMetricToNumber(v, fieldName) {
    if (v instanceof NumberLong) {
        return v.toNumber();
    }
    if (typeof v === "number") {
        return v;
    }
    assert(false, () => `metrics.shardRequests.${fieldName} is not numeric: ` + `type=${typeof v}, value=${tojson(v)}`);
}

function getShardRequestMetrics(mongos) {
    const status = assert.commandWorked(mongos.getDB("admin").serverStatus());
    assert(status.metrics, () => "serverStatus did not contain 'metrics': " + tojson(status));
    assert(
        status.metrics.shardRequests,
        () => "serverStatus did not contain 'metrics.shardRequests': " + tojson(status.metrics),
    );

    const shardMetrics = status.metrics.shardRequests;

    ["inQueue", "awaitingResponse", "complete"].forEach((field) => {
        assert(field in shardMetrics, () => `metrics.shardRequests missing '${field}': ` + tojson(shardMetrics));
    });

    return {
        inQueue: coerceMetricToNumber(shardMetrics.inQueue, "inQueue"),
        awaitingResponse: coerceMetricToNumber(shardMetrics.awaitingResponse, "awaitingResponse"),
        complete: coerceMetricToNumber(shardMetrics.complete, "complete"),
    };
}

// Common cluster setup.
const st = new ShardingTest({
    mongos: 1,
    shards: 2,
    other: {
        enableBalancer: false,
    },
});

const mongos = st.s;
const dbName = jsTestName();
const testDB = mongos.getDB(dbName);
const collName = "testColl";
const coll = testDB.getCollection(collName);

// Shard the collection so that we exercise the sharding fixed executor.
assert.commandWorked(mongos.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(mongos.adminCommand({shardCollection: coll.getFullName(), key: {x: 1}}));
assert.commandWorked(mongos.adminCommand({split: coll.getFullName(), middle: {x: 0}}));
assert.commandWorked(mongos.adminCommand({moveChunk: coll.getFullName(), find: {x: 0}, to: st.shard1.shardName}));

// Seed some data.
let bulk = coll.initializeUnorderedBulkOp();
for (let i = -20; i < 20; ++i) {
    bulk.insert({x: i, payload: "value_" + i});
}
assert.commandWorked(bulk.execute());

const kWaitTimeoutMs = 2 * 60 * 1000; // 2 minutes per assert.soon

/**
 * Single sequence: three failpoints as gates between the three counters (inQueue ->
 * awaitingResponse -> complete). We control the flow and assert metrics at each stage.
 */
(function testShardRequestMetricsSequence() {
    jsTestLog("Testing shard request metrics with three failpoints as gates");

    // 1. Set the 1st FP (BeforeQueueing) — no request can enter inQueue.
    const fp1 = configureFailPoint(mongos, "blockShardRequestBeforeQueueing");

    // 2. Wait until the 1st counter is 0, save the "before".
    assert.soon(
        () => getShardRequestMetrics(mongos).inQueue === 0,
        () => "inQueue should reach 0 with 1st FP set",
        kWaitTimeoutMs,
        200,
        {runHangAnalyzer: false},
    );
    let before1 = getShardRequestMetrics(mongos);
    jsTestLog("before1 (inQueue=0): " + tojson(before1));

    // 3. Set the 2nd FP (BeforeSending).
    const fp2 = configureFailPoint(mongos, "blockShardRequestBeforeSending");

    // 4. Send a request (parallel shell; requests will block at 1st FP). One operation is
    // enough — it may trigger multiple shard requests (e.g. to each shard).
    const awaitShell = startParallelShell(
        funWithArgs(
            function (dbName, collName) {
                const psDB = db.getSiblingDB(dbName);
                const psColl = psDB.getCollection(collName);
                try {
                    psColl.find({}).itcount();
                } catch (e) {
                    // Ignore errors; main test only cares about metrics.
                }
            },
            dbName,
            collName,
        ),
        mongos.port,
    );

    // 5. Release the 1st FP — requests can enter inQueue and block at 2nd FP.
    fp1.off();

    // 6. Wait until the 1st counter increases at least by 1.
    assert.soon(
        () => {
            const m = getShardRequestMetrics(mongos);
            return m.inQueue >= before1.inQueue + 1;
        },
        () => "inQueue should increase by at least 1 after releasing 1st FP",
        kWaitTimeoutMs,
        200,
        {runHangAnalyzer: false},
    );

    // 7. Wait until the 2nd counter is 0, save the "before".
    assert.soon(
        () => getShardRequestMetrics(mongos).awaitingResponse === 0,
        () => "awaitingResponse should reach 0 before saving before2",
        kWaitTimeoutMs,
        200,
        {runHangAnalyzer: false},
    );
    let before2 = getShardRequestMetrics(mongos);
    jsTestLog("before2 (awaitingResponse=0): " + tojson(before2));

    // 8. Set the 3rd FP (BeforeResolving).
    const fp3 = configureFailPoint(mongos, "blockShardRequestBeforeResolving");

    // 9. Release the 2nd FP — everything in inQueue flows to awaitingResponse and blocks at 3rd FP.
    fp2.off();

    // 10. Wait until the 2nd counter is at least before2.awaitingResponse + before2.inQueue.
    assert.soon(
        () => {
            const m = getShardRequestMetrics(mongos);
            return m.awaitingResponse >= before2.awaitingResponse + before2.inQueue;
        },
        () =>
            "awaitingResponse should be at least before2.awaitingResponse + before2.inQueue, " +
            "before2=" +
            tojson(before2),
        kWaitTimeoutMs,
        200,
        {runHangAnalyzer: false},
    );

    // 11. Save the "before" for the 3rd counter (complete doesn't decrease, so we just snapshot).
    let before3 = getShardRequestMetrics(mongos);
    jsTestLog("before3 (for complete check): " + tojson(before3));

    // 12. Release the 3rd FP — requests can complete.
    fp3.off();

    // 13. Wait until the 3rd counter is at least before3.complete + before3.awaitingResponse.
    assert.soon(
        () => {
            const m = getShardRequestMetrics(mongos);
            return m.complete >= before3.complete + before3.awaitingResponse;
        },
        () =>
            "complete should be at least before3.complete + before3.awaitingResponse, " + "before3=" + tojson(before3),
        kWaitTimeoutMs,
        200,
        {runHangAnalyzer: false},
    );

    awaitShell();

    const after = getShardRequestMetrics(mongos);
    jsTestLog("Metrics after sequence: " + tojson(after));
})();

st.stop();
