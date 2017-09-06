// Tests that the $changeStream stage returns an error when run against a standalone mongod.
(function() {
    "use strict";
    load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.
    // For supportsMajorityReadConcern().
    load("jstests/multiVersion/libs/causal_consistency_helpers.js");

    if (!supportsMajorityReadConcern()) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        return;
    }

    function assertChangeStreamNotSupportedOnConnection(conn) {
        const notReplicaSetErrorCode = 40573;
        assertErrorCode(
            conn.getDB("test").non_existent, [{$changeStream: {}}], notReplicaSetErrorCode);
        assertErrorCode(conn.getDB("test").non_existent,
                        [{$changeStream: {fullDocument: "updateLookup"}}],
                        notReplicaSetErrorCode);
    }

    const conn = MongoRunner.runMongod({enableMajorityReadConcern: ""});
    assert.neq(null, conn, "mongod was unable to start up");
    assertChangeStreamNotSupportedOnConnection(conn);
    assert.eq(0, MongoRunner.stopMongod(conn));

    // Test master/slave deployments.
    const masterSlaveFixture = new ReplTest("change_stream");
    const master = masterSlaveFixture.start(true, {enableMajorityReadConcern: ""});
    assertChangeStreamNotSupportedOnConnection(master);
    // Slaves start in the wrong FCV, (SERVER-31218) resulting in the wrong error code.
    // const slave = masterSlaveFixture.start(false);
    // assertChangeStreamNotSupportedOnConnection(slave);

    // Test a sharded cluster with standalone shards.
    const clusterWithStandalones = new ShardingTest(
        {shards: 2, other: {shardOptions: {enableMajorityReadConcern: ""}}, config: 1});
    // Make sure the database exists before running any commands.
    const mongosDB = clusterWithStandalones.getDB("test");
    assert.writeOK(mongosDB.unrelated.insert({}));
    assertChangeStreamNotSupportedOnConnection(clusterWithStandalones.s);
    assertChangeStreamNotSupportedOnConnection(clusterWithStandalones.shard0);
    assertChangeStreamNotSupportedOnConnection(clusterWithStandalones.shard1);
}());
