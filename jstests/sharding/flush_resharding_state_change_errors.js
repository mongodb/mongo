/**
 * Tests that _flushReshardingStateChange command retries sharding metadata refresh on transient
 * errors until there is a failover.
 */
import {configureFailPoint} from 'jstests/libs/fail_point_util.js';
import {Thread} from 'jstests/libs/parallelTester.js';
import {ShardingTest} from 'jstests/libs/shardingtest.js';

function runMoveCollection(host, ns, toShard) {
    const mongos = new Mongo(host);
    return mongos.adminCommand({moveCollection: ns, toShard});
}

function getFlushReshardingStateChangeMetrics(conn) {
    const shardingStatistics =
        assert.commandWorked(conn.adminCommand({serverStatus: 1})).shardingStatistics;
    return {
        countFlushReshardingStateChangeTotalShardingMetadataRefreshes:
            shardingStatistics.countFlushReshardingStateChangeTotalShardingMetadataRefreshes,
        countFlushReshardingStateChangeSuccessfulShardingMetadataRefreshes:
            shardingStatistics.countFlushReshardingStateChangeSuccessfulShardingMetadataRefreshes,
        countFlushReshardingStateChangeFailedShardingMetadataRefreshes:
            shardingStatistics.countFlushReshardingStateChangeFailedShardingMetadataRefreshes
    };
}

function validateFlushReshardingStateChangeMetrics(metrics) {
    assert.gte(metrics.countFlushReshardingStateChangeTotalShardingMetadataRefreshes, 0, metrics);
    assert.gte(
        metrics.countFlushReshardingStateChangeSuccessfulShardingMetadataRefreshes, 0, metrics);
    assert.gte(metrics.countFlushReshardingStateChangeFailedShardingMetadataRefreshes, 0, metrics);
    assert.gte(metrics.countFlushReshardingStateChangeTotalShardingMetadataRefreshes,
               metrics.countFlushReshardingStateChangeSuccessfulShardingMetadataRefreshes +
                   metrics.countFlushReshardingStateChangeFailedShardingMetadataRefreshes,
               metrics);
}

function assertSoonFlushReshardingStateChangeStartRetryingOnRefreshErrors(conn) {
    let numTries = 0;
    assert.soon(() => {
        numTries++;
        const metrics = getFlushReshardingStateChangeMetrics(conn);
        validateFlushReshardingStateChangeMetrics(metrics);
        if (numTries % 100 == 0) {
            jsTest.log("Waiting for _flushReshardingStateChange to hit refresh errors: " +
                       tojson(metrics));
        }
        return metrics.countFlushReshardingStateChangeTotalShardingMetadataRefreshes > 1 &&
            metrics.countFlushReshardingStateChangeFailedShardingMetadataRefreshes > 0;
    });
}

function assertSoonFlushReshardingStateChangeStopRetryingOnRefreshErrors(conn) {
    let numTries = 0;
    let prevMetrics;

    // Use a large interval to decrease the chance of checking metrics before the next refresh
    // retry.
    const timeout = null;  // Use the default timeout.
    const interval = 1000;
    assert.soon(() => {
        numTries++;
        const currMetrics = getFlushReshardingStateChangeMetrics(conn);
        validateFlushReshardingStateChangeMetrics(currMetrics);
        if (numTries % 10 == 0) {
            jsTest.log("Waiting for _flushReshardingStateChange to stop refreshing: " +
                       tojson({conn, currMetrics, prevMetrics}));
        }
        if (bsonWoCompare(prevMetrics, currMetrics) == 0) {
            jsTest.log("Finished waiting for _flushReshardingStateChange to stop refreshing: " +
                       tojson({conn, currMetrics, prevMetrics}));
            return true;
        }
        prevMetrics = currMetrics;
        return false;
    }, "Timed out waiting for _flushReshardingStateChange to stop refreshing", timeout, interval);
}

function assertFlushReshardingStateChangeMetricsNoRefreshErrors(conn) {
    const metrics = getFlushReshardingStateChangeMetrics(conn);
    jsTest.log("Checking _flushReshardingStateChange metrics: " + tojson(metrics));
    validateFlushReshardingStateChangeMetrics(metrics);
    assert.eq(metrics.countFlushReshardingStateChangeFailedShardingMetadataRefreshes, 0, metrics);
}

function stepUpNewPrimary(rst) {
    const oldPrimary = rst.getPrimary();
    const oldSecondary = rst.getSecondary();
    assert.neq(oldPrimary, oldSecondary);
    rst.stepUp(rst.getSecondary(), {awaitReplicationBeforeStepUp: false});
    const newPrimary = rst.getPrimary();
    assert.eq(newPrimary, oldSecondary);
}

function testRetryOnTransientError(st, {enableCloneNoRefresh}) {
    jsTest.log("Start testing that _flushReshardingStateChange retries sharding metadata refresh " +
               "on transient error " + tojsononeline({enableCloneNoRefresh}));
    // Set up the collection to reshard.
    const dbName = "testDbBasic";
    const collName = "testColl";
    const ns = dbName + '.' + collName;
    const testColl = st.s.getCollection(ns);
    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
    assert.commandWorked(testColl.insert([{x: -1}, {x: 0}, {x: 1}]));
    assert.commandWorked(testColl.createIndex({x: 1}));

    // Set activation probability to less than 1 so that as long as there are retries,
    // moveCollection will eventually succeed.
    let activationProbability = 0.5;
    let fp0 = configureFailPoint(st.rs0.getPrimary(),
                                 "failFlushReshardingStateChange",
                                 {errorCode: ErrorCodes.WriteConcernTimeout},
                                 {activationProbability});
    let fp1 = configureFailPoint(st.rs1.getPrimary(),
                                 "failFlushReshardingStateChange",
                                 {errorCode: ErrorCodes.WriteConcernTimeout},
                                 {activationProbability});

    const moveThread = new Thread(runMoveCollection, st.s.host, ns, st.shard1.shardName);
    moveThread.start();

    jsTest.log("Start waiting for moveCollection to finish");
    assert.commandWorked(moveThread.returnData());
    jsTest.log("Finished waiting for moveCollection to finish");

    fp0.off();
    fp1.off();
}

function testStopRetryingOnFailover(st, {enableCloneNoRefresh}) {
    jsTest.log("Start testing that _flushReshardingStateChange stops retrying sharding metadata " +
               "refresh on failover " + tojsononeline({enableCloneNoRefresh}));

    // Set up the collection to reshard.
    const dbName = "testDbStopRetrying";
    const collName = "testColl";
    const ns = dbName + '.' + collName;
    const testColl = st.s.getCollection(ns);
    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
    assert.commandWorked(testColl.insert([{x: -1}, {x: 0}, {x: 1}]));
    assert.commandWorked(testColl.createIndex({x: 1}));

    const primary0BeforeFailover = st.rs0.getPrimary();
    const primary1BeforeFailover = st.rs1.getPrimary();
    let fp0 = configureFailPoint(primary0BeforeFailover,
                                 "failFlushReshardingStateChange",
                                 {errorCode: ErrorCodes.WriteConcernTimeout});
    let fp1 = configureFailPoint(primary1BeforeFailover,
                                 "failFlushReshardingStateChange",
                                 {errorCode: ErrorCodes.WriteConcernTimeout});

    const moveThread = new Thread(runMoveCollection, st.s.host, ns, st.shard1.shardName);
    moveThread.start();

    jsTest.log(
        "Waiting for _flushReshardingStateChange on shard0 to start retrying on refresh errors");
    assertSoonFlushReshardingStateChangeStartRetryingOnRefreshErrors(primary0BeforeFailover);
    jsTest.log(
        "Waiting for _flushReshardingStateChange to shard1 to start retrying on refresh errors");
    assertSoonFlushReshardingStateChangeStartRetryingOnRefreshErrors(primary1BeforeFailover);

    jsTest.log("Triggering a failover on shard0");
    stepUpNewPrimary(st.rs0);
    const primary0AfterFailover = st.rs0.getPrimary();
    jsTest.log("Triggering a failover on shard1");
    stepUpNewPrimary(st.rs1);
    const primary1AfterFailover = st.rs1.getPrimary();

    jsTest.log("Checking that _flushReshardingStateChange retries eventually stop after failover");
    assertSoonFlushReshardingStateChangeStopRetryingOnRefreshErrors(primary0BeforeFailover);
    assertSoonFlushReshardingStateChangeStopRetryingOnRefreshErrors(primary1BeforeFailover);

    jsTest.log("Start waiting for moveCollection to finish");
    assert.commandWorked(moveThread.returnData());
    jsTest.log("Finished waiting for moveCollection to finish");

    assertFlushReshardingStateChangeMetricsNoRefreshErrors(primary0AfterFailover);
    assertFlushReshardingStateChangeMetricsNoRefreshErrors(primary1AfterFailover);

    fp0.off();
    fp1.off();
}

function runTests({enableCloneNoRefresh}) {
    jsTest.log("Start testing with " + tojsononeline({enableCloneNoRefresh}));
    const st = new ShardingTest({
        shards: 2,
        rs: {
            nodes: 3,
            setParameter: {
                featureFlagReshardingCloneNoRefresh: enableCloneNoRefresh,
            }
        },
        other: {
            configOptions: {
                setParameter: {
                    featureFlagReshardingCloneNoRefresh: enableCloneNoRefresh,
                }
            }
        }
    });

    testRetryOnTransientError(st, {enableCloneNoRefresh});
    testStopRetryingOnFailover(st, {enableCloneNoRefresh});

    st.stop();
}

runTests({enableCloneNoRefresh: false});
runTests({enableCloneNoRefresh: true});
