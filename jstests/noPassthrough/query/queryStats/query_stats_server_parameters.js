/**
 * Test that the server parameters configuring queryStats work properly under various conditions.
 * @tags: [requires_fcv_82]
 */
import {getExecCount, resetQueryStatsStore} from "jstests/libs/query/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function runTest(conn) {
    const testDB = conn.getDB('test');
    var coll = testDB[jsTestName()];
    coll.drop();

    const bulk = coll.initializeUnorderedBulkOp();
    const numDocs = 100;
    for (let i = 0; i < numDocs / 2; ++i) {
        bulk.insert({foo: "ABCDE"});
        bulk.insert({foo: "CDEFG"});
    }
    assert.commandWorked(bulk.execute());

    function assertSampleCount(expectedCount, totalExec = 10) {
        for (let i = 0; i < totalExec; ++i) {
            coll.find({foo: "bar"}).itcount();
        }
        assert.eq(getExecCount(testDB, coll.getName()), expectedCount);
    }

    {  // Test sample-based rate limiting.
        resetQueryStatsStore(conn, "1MB");
        conn.adminCommand({setParameter: 1, internalQueryStatsSampleRate: 1.0});
        conn.adminCommand({setParameter: 1, internalQueryStatsRateLimit: 0});
        assertSampleCount(10);
    }

    {  // Test window-based rate limiting.
        resetQueryStatsStore(conn, "1MB");
        conn.adminCommand({setParameter: 1, internalQueryStatsSampleRate: 0.0});
        conn.adminCommand({setParameter: 1, internalQueryStatsRateLimit: 3});
        assertSampleCount(3);
    }

    {  // Test sampling-based takes precedence over window-based rate limiting.
        resetQueryStatsStore(conn, "1MB");
        conn.adminCommand({setParameter: 1, internalQueryStatsSampleRate: 1.0});
        conn.adminCommand({setParameter: 1, internalQueryStatsRateLimit: 1});
        assertSampleCount(10);
    }

    {  // Test idempotency of setParameter commands.
        resetQueryStatsStore(conn, "1MB");
        conn.adminCommand({setParameter: 1, internalQueryStatsSampleRate: 1.0});
        conn.adminCommand({setParameter: 1, internalQueryStatsRateLimit: 0});
        assertSampleCount(10);

        // Reapply the same parameters in different order.
        resetQueryStatsStore(conn, "1MB");
        conn.adminCommand({setParameter: 1, internalQueryStatsRateLimit: 0});
        conn.adminCommand({setParameter: 1, internalQueryStatsSampleRate: 1.0});
        assertSampleCount(10);
    }

    {  // Test query stats is disabled when both parameters are set to 0.
        resetQueryStatsStore(conn, "1MB");
        conn.adminCommand({setParameter: 1, internalQueryStatsSampleRate: 0.0});
        conn.adminCommand({setParameter: 1, internalQueryStatsRateLimit: 0});
        assertSampleCount(0);
    }

    {  // Test sample-based rate limiting with 50% sampling rate.
        resetQueryStatsStore(conn, "1MB");
        conn.adminCommand({setParameter: 1, internalQueryStatsSampleRate: 0.5});
        conn.adminCommand({setParameter: 1, internalQueryStatsRateLimit: 0});
        for (let i = 0; i < 100; ++i) {
            coll.find({foo: "bar"}).itcount();
        }
        const execCount = getExecCount(testDB, coll.getName());

        // With 50% sampling rate, the chance of the sample count failling between 10 and 90 is
        // over 99.9999999%. This test should be very stable.
        assert.gte(execCount, 10);
        assert.lte(execCount, 90);
    }
}

const conn = MongoRunner.runMongod();
runTest(conn);
MongoRunner.stopMongod(conn);

const st = new ShardingTest({
    mongos: 1,
    shards: 1,
    config: 1,
    rs: {nodes: 1},
});
runTest(st.s);
st.stop();