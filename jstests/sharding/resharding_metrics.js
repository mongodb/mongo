/**
 * Tests the basic functionality of the resharding metrics section in server status.
 *
 * @tags: [requires_fcv_47]
 */

(function() {
'use strict';

const kDbName = "resharding_metrics";

function testMetricsArePresent(mongo) {
    const stats = mongo.getDB(kDbName).serverStatus({});
    assert(stats.hasOwnProperty('shardingStatistics'), stats);
    const shardingStats = stats.shardingStatistics;
    assert(shardingStats.hasOwnProperty('resharding'),
           `Missing resharding section in ${tojson(shardingStats)}`);

    function verifyMetric(metrics, tag, expectedValue) {
        assert(metrics.hasOwnProperty(tag), `Missing ${tag} in ${tojson(metrics)}`);
        assert.eq(metrics[tag],
                  expectedValue,
                  `Expected the value for ${tag} to be ${expectedValue}: ${tojson(metrics)}`);
    }

    const metrics = shardingStats.resharding;
    verifyMetric(metrics, "successfulOperations", 0);
    verifyMetric(metrics, "failedOperations", 0);
    verifyMetric(metrics, "canceledOperations", 0);
    verifyMetric(metrics, "documentsCopied", 0);
    verifyMetric(metrics, "bytesCopied", 0);
    verifyMetric(metrics, "oplogEntriesApplied", 0);
    verifyMetric(metrics, "countWritesDuringCriticalSection", 0);
}

const st = new ShardingTest({mongos: 1, config: 1, shards: 1, rs: {nodes: 1}});

testMetricsArePresent(st.rs0.getPrimary());
testMetricsArePresent(st.configRS.getPrimary());

st.stop();
})();
