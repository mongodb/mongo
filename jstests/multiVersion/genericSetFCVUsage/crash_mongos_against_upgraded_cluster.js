// A mongos participating in a cluster that has been upgraded both a binary and FCV version above it
// should crash.
//
// This kind of scenario can happen when a user forgets to upgrade the mongos binary and then calls
// setFCV(upgrade), leaving the still downgraded mongos unable to communicate. Rather than the
// mongos logging incompatible server version errors endlessly, we've chosen to crash it.

TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {
"use strict";

function runTest(downgradeVersion) {
    jsTestLog("Running test with downgradeVersion: " + downgradeVersion);
    const downgradeFCV = binVersionToFCV(downgradeVersion);
    let st = new ShardingTest({mongos: 1, shards: 1});
    const ns = "testDB.testColl";
    let mongosAdminDB = st.s.getDB("admin");

    // Assert that a mongos using the downgraded binary version will crash when connecting to a
    // cluster running on the 'latest' binary version with the 'latest' FCV.
    let downgradedMongos =
        MongoRunner.runMongos({configdb: st.configRS.getURL(), binVersion: downgradeVersion});

    assert(!downgradedMongos);

    // Assert that a mongos using the downgraded binary version will successfully connect to a
    // cluster running on the 'latest' binary version with the downgraded FCV.
    assert.commandWorked(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: downgradeFCV}));

    // wait until all config server nodes are downgraded
    // awaitReplication waits for all secondaries to replicate primary's latest opTime which will
    // guarantee propagation of the write to the admin.system.version collection which triggers the
    // change FCV.
    st.configRS.awaitReplication();

    downgradedMongos =
        MongoRunner.runMongos({configdb: st.configRS.getURL(), binVersion: downgradeVersion});
    assert.neq(null,
               downgradedMongos,
               "mongos was unable to start up with binary version=" + downgradeVersion +
                   " and connect to FCV=" + downgradeFCV + " cluster");

    // Ensure that the 'downgradeVersion' binary mongos can perform reads and writes to the shards
    // in the cluster.
    assert.commandWorked(downgradedMongos.getDB("test").foo.insert({x: 1}));
    let foundDoc = downgradedMongos.getDB("test").foo.findOne({x: 1});
    assert.neq(null, foundDoc);
    assert.eq(1, foundDoc.x, tojson(foundDoc));

    // Assert that the 'downgradeVersion' binary mongos will crash after the cluster is upgraded to
    // 'latestFCV'.
    assert.commandWorked(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
    let error = assert.throws(function() {
        downgradedMongos.getDB("test").foo.insert({x: 1});
    });
    assert(isNetworkError(error));
    assert(!downgradedMongos.conn);

    st.stop();
}

runTest('last-continuous');
runTest('last-lts');
})();
