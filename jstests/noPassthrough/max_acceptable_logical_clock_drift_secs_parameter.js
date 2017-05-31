/**
 * Tests what values are accepted for the maxAcceptableLogicalClockDriftSecs startup parameter, and
 * that servers in a sharded clusters reject logical times more than
 * maxAcceptableLogicalClockDriftSecs ahead of their wall clocks.
 */
(function() {
    "use strict";

    // maxAcceptableLogicalClockDriftSecs cannot be negative, zero, or a non-number.
    let conn = MongoRunner.runMongod({setParameter: {maxAcceptableLogicalClockDriftSecs: -1}});
    assert.eq(null, conn, "expected server to reject negative maxAcceptableLogicalClockDriftSecs");

    conn = MongoRunner.runMongod({setParameter: {maxAcceptableLogicalClockDriftSecs: 0}});
    assert.eq(null, conn, "expected server to reject zero maxAcceptableLogicalClockDriftSecs");

    conn = MongoRunner.runMongod({setParameter: {maxAcceptableLogicalClockDriftSecs: "value"}});
    assert.eq(
        null, conn, "expected server to reject non-numeric maxAcceptableLogicalClockDriftSecs");

    conn = MongoRunner.runMongod(
        {setParameter: {maxAcceptableLogicalClockDriftSecs: new Timestamp(50, 0)}});
    assert.eq(
        null, conn, "expected server to reject non-numeric maxAcceptableLogicalClockDriftSecs");

    // Any positive number is valid.
    conn = MongoRunner.runMongod({setParameter: {maxAcceptableLogicalClockDriftSecs: 1}});
    assert.neq(null, conn, "failed to start mongod with valid maxAcceptableLogicalClockDriftSecs");
    MongoRunner.stopMongod(conn);

    conn = MongoRunner.runMongod({
        setParameter: {maxAcceptableLogicalClockDriftSecs: 60 * 60 * 24 * 365 * 10}
    });  // 10 years.
    assert.neq(null, conn, "failed to start mongod with valid maxAcceptableLogicalClockDriftSecs");
    MongoRunner.stopMongod(conn);

    // Verify maxAcceptableLogicalClockDriftSecs works as expected in a sharded cluster.
    const maxDriftValue = 100;
    const st = new ShardingTest({
        shards: 1,
        shardOptions: {setParameter: {maxAcceptableLogicalClockDriftSecs: maxDriftValue}},
        mongosOptions: {setParameter: {maxAcceptableLogicalClockDriftSecs: maxDriftValue}}
    });
    let testDB = st.s.getDB("test");

    // Contact cluster to get initial logical time.
    let res = assert.commandWorked(testDB.runCommand({isMaster: 1}));
    let lt = res.$logicalTime;

    // Try to advance logical time by more than the max acceptable drift, which should fail the rate
    // limiter.
    let tooFarTime = Object.assign(
        lt, {clusterTime: new Timestamp(lt.clusterTime.getTime() + (maxDriftValue * 2), 0)});
    assert.commandFailedWithCode(testDB.runCommand({isMaster: 1, $logicalTime: tooFarTime}),
                                 ErrorCodes.ClusterTimeFailsRateLimiter,
                                 "expected command to not pass the rate limiter");

    st.stop();
})();
