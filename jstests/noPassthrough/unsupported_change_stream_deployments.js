// Tests that the $changeStream stage returns an error when run against a standalone mongod.
(function() {
    "use strict";
    load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.
    // For supportsMajorityReadConcern().
    load("jstests/multiVersion/libs/causal_consistency_helpers.js");
    load("jstests/libs/feature_compatibility_version.js");  // For checkFCV.

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
    // $changeStream cannot run on a non-existent database.
    assert.writeOK(conn.getDB("test").ensure_db_exists.insert({}));
    assertChangeStreamNotSupportedOnConnection(conn);
    assert.eq(0, MongoRunner.stopMongod(conn));

    // Test master/slave deployments.
    const masterSlaveFixture = new ReplTest("change_stream");
    const master = masterSlaveFixture.start(true, {enableMajorityReadConcern: ""});
    assert.writeOK(master.getDB("test").ensure_db_exists.insert({}));
    assertChangeStreamNotSupportedOnConnection(master);

    const slave = masterSlaveFixture.start(false);
    // Slaves start in FCV 3.4; we need to wait for it to sync the FCV document from the master
    // before trying a change stream, or the change stream will fail for the wrong reason.
    assert.soonNoExcept(() => checkFCV(slave.getDB("admin"), "3.6") || true);
    assert.soonNoExcept(() => slave.getDB("test").ensure_db_exists.exists());
    assertChangeStreamNotSupportedOnConnection(slave);

    // Test a sharded cluster with standalone shards.
    const clusterWithStandalones = new ShardingTest(
        {shards: 2, other: {shardOptions: {enableMajorityReadConcern: ""}}, config: 1});
    // Make sure the database exists before running any commands.
    const mongosDB = clusterWithStandalones.getDB("test");
    // enableSharding will create the db at the cluster level but not on the shards. $changeStream
    // through mongoS will be allowed to run on the shards despite the lack of a database.
    assert.commandWorked(mongosDB.adminCommand({enableSharding: "test"}));
    assertChangeStreamNotSupportedOnConnection(clusterWithStandalones.s);
    // Shard the 'ensure_db_exists' collection on a hashed key before running $changeStream on the
    // shards directly. This will ensure that the database is created on both shards.
    assert.commandWorked(
        mongosDB.adminCommand({shardCollection: "test.ensure_db_exists", key: {_id: "hashed"}}));
    assertChangeStreamNotSupportedOnConnection(clusterWithStandalones.shard0);
    assertChangeStreamNotSupportedOnConnection(clusterWithStandalones.shard1);
}());
