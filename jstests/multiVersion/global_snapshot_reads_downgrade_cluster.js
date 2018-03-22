/**
 * Test the downgrade of a sharded cluster from latest to last-stable version succeeds, verifying
 * the behavior of global snapshot reads throughout the process.
 */

// Checking UUID consistency uses cached connections, which are not valid across restarts or
// stepdowns.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {
    "use strict";

    load("jstests/libs/feature_compatibility_version.js");
    load("jstests/multiVersion/libs/multi_rs.js");
    load("jstests/multiVersion/libs/multi_cluster.js");

    const conn = MongoRunner.runMongod();
    const supportsSnapshotReadConcern =
        conn.getDB("test").serverStatus().storageEngine.supportsSnapshotReadConcern;
    MongoRunner.stopMongod(conn);

    if (!supportsSnapshotReadConcern) {
        jsTestLog("Skipping test since storage engine doesn't support snapshot read concern.");
        return;
    }

    let txnNumber = 0;  // Global counter used and incremented for all snapshot reads.

    /**
     * Runs the given command on the given database, asserting the command failed or succeeded
     * depending on the value of expectSuccess.
     */
    function runCommandAndVerifyResponse(sessionDb, cmdObj, expectSuccess, expectedCode) {
        const res = sessionDb.runCommand(cmdObj);

        if (expectSuccess) {
            // A snapshot read may fail with SnapshotTooOld in 4.0. This is acceptable for this
            // test, since it does not verify the ability to consistently establish a snapshot, only
            // that attempts to establish one aren't rejected.
            if (!res.ok) {
                assert.eq(ErrorCodes.SnapshotTooOld, res.code);
                return;
            }

            assert.commandWorked(res, "expected command to succeed, cmd: " + tojson(cmdObj));
        } else {
            assert.commandFailedWithCode(res,
                                         expectedCode,
                                         "command did not fail with expected error code, cmd: " +
                                             tojson(cmdObj) + ", expectedCode: " +
                                             tojson(expectedCode));
        }
    }

    /**
     * Runs reads with snapshot readConcern against mongos, expecting they either fail or succeed
     * depending on the expectSuccess parameter.
     */
    function verifyGlobalSnapshotReads(conn, expectSuccess, expectedCode) {
        const session = conn.startSession({causalConsistency: false});

        // Unsharded collection.
        const unshardedDb = session.getDatabase("unshardedDb");
        runCommandAndVerifyResponse(unshardedDb,
                                    {
                                      find: "unsharded",
                                      readConcern: {level: "snapshot"},
                                      txnNumber: NumberLong(txnNumber++)
                                    },
                                    expectSuccess,
                                    expectedCode);

        // Sharded collection, one shard.
        const shardedDb = session.getDatabase("shardedDb");
        runCommandAndVerifyResponse(shardedDb,
                                    {
                                      find: "sharded",
                                      filter: {x: 1},
                                      readConcern: {level: "snapshot"},
                                      txnNumber: NumberLong(txnNumber++)
                                    },
                                    expectSuccess,
                                    expectedCode);

        // Sharded collection, all shards.
        runCommandAndVerifyResponse(
            shardedDb,
            {find: "sharded", readConcern: {level: "snapshot"}, txnNumber: NumberLong(txnNumber++)},
            expectSuccess,
            expectedCode);
    }

    // Start a cluster with two shards and two mongos at the latest version.
    var st = new ShardingTest({
        shards: 2,
        mongos: 2,
        other: {
            mongosOptions: {binVersion: "latest"},
            configOptions: {binVersion: "latest"},
            rsOptions: {binVersion: "latest"},
        },
        rs: {nodes: 3}  // Use 3 node replica sets to allow downgrades with no downtime.
    });
    checkFCV(st.configRS.getPrimary().getDB("admin"), latestFCV);

    // Setup a sharded collection with two chunks, one on each shard.
    assert.commandWorked(st.s.adminCommand({enableSharding: "shardedDb"}));
    st.ensurePrimaryShard("shardedDb", st.shard0.shardName);
    assert.commandWorked(st.s.adminCommand({shardCollection: "shardedDb.sharded", key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({split: "shardedDb.sharded", middle: {x: 0}}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: "shardedDb.sharded", find: {x: 1}, to: st.shard1.shardName}));

    // Insert some data for the reads to find.
    st.s.getDB("unshardedDb").unsharded.insert({x: 1});
    st.s.getDB("shardedDb").sharded.insert([{x: -1}, {x: 1}]);

    // Global snapshot reads are accepted.
    verifyGlobalSnapshotReads(st.s0, true);
    verifyGlobalSnapshotReads(st.s1, true);

    // Downgrade featureCompatibilityVersion to 3.6.
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
    checkFCV(st.configRS.getPrimary().getDB("admin"), lastStableFCV);

    // Global snapshot reads are accepted.
    verifyGlobalSnapshotReads(st.s0, true);
    verifyGlobalSnapshotReads(st.s1, true);

    // Downgrade the mongos servers first.
    jsTest.log("Downgrading mongos servers.");
    st.upgradeCluster("last-stable",
                      {upgradeConfigs: false, upgradeMongos: true, upgradeShards: false});

    // Global snapshot reads are rejected with InvalidOptions, because downgraded mongos will not
    // forward the txnNumber to the shards.
    verifyGlobalSnapshotReads(st.s0, false, ErrorCodes.InvalidOptions);
    verifyGlobalSnapshotReads(st.s1, false, ErrorCodes.InvalidOptions);

    // Then downgrade the shard servers.
    jsTest.log("Downgrading shard servers.");
    st.upgradeCluster("last-stable",
                      {upgradeConfigs: false, upgradeMongos: false, upgradeShards: true});

    // Global snapshot reads are rejected with FailedToParse, because the shards will reject the
    // unknown readConcern field.
    verifyGlobalSnapshotReads(st.s0, false, ErrorCodes.FailedToParse);
    verifyGlobalSnapshotReads(st.s1, false, ErrorCodes.FailedToParse);

    // Finally, downgrade the config servers.
    jsTest.log("Downgrading config servers.");
    st.upgradeCluster("last-stable",
                      {upgradeConfigs: true, upgradeMongos: false, upgradeShards: false});

    // Global snapshot reads are rejected with FailedToParse, because the shards will reject the
    // unknown readConcern field.
    verifyGlobalSnapshotReads(st.s0, false, ErrorCodes.FailedToParse);
    verifyGlobalSnapshotReads(st.s1, false, ErrorCodes.FailedToParse);

    st.stop();
})();
