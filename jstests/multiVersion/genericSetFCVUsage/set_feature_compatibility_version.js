/**
 * Tests setFeatureCompatibilityVersion.
 */

// Checking UUID consistency involves talking to a shard node, which in this test is shutdown
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

// A replset test case checks that replication to a secondary ceases, so we do not expect identical
// data.
TestData.skipCheckDBHashes = true;

(function() {
"use strict";

load("jstests/libs/get_index_helpers.js");
load("jstests/libs/write_concern_util.js");
load("jstests/replsets/rslib.js");

let dbpath = MongoRunner.dataPath + "feature_compatibility_version";
resetDbpath(dbpath);
let res;

const latest = "latest";

function runStandaloneTest(downgradeVersion) {
    jsTestLog("Running standalone test with 'downgradeVersion': " + downgradeVersion);
    const downgradeFCV = binVersionToFCV(downgradeVersion);

    let conn;
    let adminDB;

    // A 'latest' binary standalone should default to 'latestFCV'.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest});
    assert.neq(
        null, conn, "mongod was unable to start up with version=" + latest + " and no data files");
    adminDB = conn.getDB("admin");
    checkFCV(adminDB, latestFCV);

    jsTestLog("EXPECTED TO FAIL: featureCompatibilityVersion cannot be set to an invalid value");
    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: 5}));
    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: "3.2"}));
    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: "4.8"}));
    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: "3.4"}));

    jsTestLog("EXPECTED TO FAIL: setFeatureCompatibilityVersion rejects unknown fields.");
    assert.commandFailed(
        adminDB.runCommand({setFeatureCompatibilityVersion: downgradeFCV, unknown: 1}));

    jsTestLog(
        "EXPECTED TO FAIL: setFeatureCompatibilityVersion only accepts downgradeOnDiskChanges " +
        " parameter when downgrading to last-continuous FCV");
    assert.commandFailedWithCode(
        adminDB.runCommand(
            {setFeatureCompatibilityVersion: latestFCV, downgradeOnDiskChanges: true}),
        ErrorCodes.IllegalOperation);

    assert.commandFailedWithCode(
        adminDB.runCommand(
            {setFeatureCompatibilityVersion: latestFCV, downgradeOnDiskChanges: false}),
        ErrorCodes.IllegalOperation);

    if (downgradeFCV === lastLTSFCV && lastContinuousFCV != lastLTSFCV) {
        assert.commandFailedWithCode(
            adminDB.runCommand(
                {setFeatureCompatibilityVersion: downgradeFCV, downgradeOnDiskChanges: true}),
            ErrorCodes.IllegalOperation);
        assert.commandFailedWithCode(
            adminDB.runCommand(
                {setFeatureCompatibilityVersion: downgradeFCV, downgradeOnDiskChanges: false}),
            ErrorCodes.IllegalOperation);
    } else {
        jsTestLog("Test that setFeatureCompatibilityVersion succeeds with downgradeOnDiskChanges " +
                  "parameter when FCV is last-continuous");
        assert.commandWorked(adminDB.runCommand(
            {setFeatureCompatibilityVersion: downgradeFCV, downgradeOnDiskChanges: true}));
        checkFCV(adminDB, downgradeFCV);
        checkLog.contains(conn, "Downgrading on-disk format");
        assert.commandWorked(adminDB.runCommand({clearLog: 'global'}));

        // Upgrade still fails with downgradeOnDiskChanges: true.
        assert.commandFailedWithCode(
            adminDB.runCommand(
                {setFeatureCompatibilityVersion: latestFCV, downgradeOnDiskChanges: true}),
            ErrorCodes.IllegalOperation);

        // Set the FCV back to 'latest'.
        assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
        checkFCV(adminDB, latestFCV);

        assert.commandWorked(adminDB.runCommand(
            {setFeatureCompatibilityVersion: downgradeFCV, downgradeOnDiskChanges: false}));
        checkFCV(adminDB, downgradeFCV);

        // Upgrade still fails with downgradeOnDiskChanges: false.
        assert.commandFailedWithCode(
            adminDB.runCommand(
                {setFeatureCompatibilityVersion: latestFCV, downgradeOnDiskChanges: false}),
            ErrorCodes.IllegalOperation);

        // Set the FCV back to 'latest'.
        assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
        checkFCV(adminDB, latestFCV);
    }

    jsTestLog(
        "EXPECTED TO FAIL: setFeatureCompatibilityVersion can only be run on the admin database");
    assert.commandFailed(
        conn.getDB("test").runCommand({setFeatureCompatibilityVersion: downgradeFCV}));

    jsTestLog("EXPECTED TO FAIL: featureCompatibilityVersion cannot be set via setParameter");
    assert.commandFailed(
        adminDB.runCommand({setParameter: 1, featureCompatibilityVersion: downgradeFCV}));

    // setFeatureCompatibilityVersion fails to downgrade FCV if the write fails.
    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: "failCollectionUpdates",
        data: {collectionNS: "admin.system.version"},
        mode: "alwaysOn"
    }));
    jsTestLog(
        "EXPECTED TO FAIL: setFeatureCompatibilityVersion fails to downgrade FCV if the write fails");
    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: downgradeFCV}));
    checkFCV(adminDB, latestFCV);
    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: "failCollectionUpdates",
        data: {collectionNS: "admin.system.version"},
        mode: "off"
    }));

    // featureCompatibilityVersion can be downgraded to 'downgradeFCV'.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: downgradeFCV}));
    checkFCV(adminDB, downgradeFCV);

    // setFeatureCompatibilityVersion does not support upgrading/downgrading between last-lts and
    // last-continuous FCV by default. Upgrading from last-lts to last-continuous is allowed if
    // fromConfigServer is set to true.
    if (lastContinuousFCV !== lastLTSFCV) {
        if (downgradeFCV === lastContinuousFCV) {
            // Attempt to downgrade FCV from last-continuous to last-lts.
            assert.commandFailedWithCode(
                adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV}),
                ErrorCodes.IllegalOperation);
            // Downgrading from last-continuous to last-lts is not allowed even with
            // fromConfigServer: true.
            assert.commandFailedWithCode(
                adminDB.runCommand(
                    {setFeatureCompatibilityVersion: lastLTSFCV, fromConfigServer: true}),
                ErrorCodes.IllegalOperation);
        } else {
            // Attempt to upgrade FCV from last-lts to last-continuous.
            assert.commandFailedWithCode(
                adminDB.runCommand({setFeatureCompatibilityVersion: lastContinuousFCV}), 5070603);

            assert.commandFailedWithCode(
                adminDB.runCommand(
                    {setFeatureCompatibilityVersion: lastContinuousFCV, fromConfigServer: false}),
                5070603);
            assert.commandWorked(adminDB.runCommand(
                {setFeatureCompatibilityVersion: lastContinuousFCV, fromConfigServer: true}));
            checkFCV(adminDB, lastContinuousFCV);

            // Reset the FCV back to last-lts.
            assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
            checkFCV(adminDB, latestFCV);
            assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
            checkFCV(adminDB, lastLTSFCV);
        }
    }

    // setFeatureCompatibilityVersion fails to upgrade to 'latestFCV' if the write fails.
    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: "failCollectionUpdates",
        data: {collectionNS: "admin.system.version"},
        mode: "alwaysOn"
    }));
    jsTestLog(
        "EXPECTED TO FAIL: setFeatureCompatibilityVersion fails to upgrade to 'latestFCV' if the write fails");
    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
    checkFCV(adminDB, downgradeFCV);
    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: "failCollectionUpdates",
        data: {collectionNS: "admin.system.version"},
        mode: "off"
    }));

    // featureCompatibilityVersion can be upgraded to 'latestFCV'.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
    checkFCV(adminDB, latestFCV);

    MongoRunner.stopMongod(conn);

    // featureCompatibilityVersion is durable.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest});
    assert.neq(
        null, conn, "mongod was unable to start up with version=" + latest + " and no data files");
    adminDB = conn.getDB("admin");
    checkFCV(adminDB, latestFCV);
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: downgradeFCV}));
    checkFCV(adminDB, downgradeFCV);
    MongoRunner.stopMongod(conn);

    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest, noCleanData: true});
    assert.neq(null,
               conn,
               "mongod was unable to start up with binary version=" + latest +
                   " and featureCompatibilityVersion=" + downgradeFCV);
    adminDB = conn.getDB("admin");
    checkFCV(adminDB, downgradeFCV);
    MongoRunner.stopMongod(conn);

    // If you upgrade from 'downgradeVersion' binary to 'latest' binary and have non-local
    // databases, FCV remains 'downgradeFCV'.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: downgradeVersion});
    assert.neq(
        null,
        conn,
        "mongod was unable to start up with version=" + downgradeVersion + " and no data files");
    assert.commandWorked(conn.getDB("test").coll.insert({a: 5}));
    adminDB = conn.getDB("admin");
    checkFCV(adminDB, downgradeFCV);
    MongoRunner.stopMongod(conn);

    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest, noCleanData: true});
    assert.neq(null,
               conn,
               "mongod was unable to start up with binary version=" + latest +
                   " and featureCompatibilityVersion=" + downgradeFCV);
    adminDB = conn.getDB("admin");
    checkFCV(adminDB, downgradeFCV);
    MongoRunner.stopMongod(conn);

    // A 'latest' binary mongod started with --shardsvr and clean data files defaults to
    // lastLTSFCV.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest, shardsvr: ""});
    assert.neq(
        null, conn, "mongod was unable to start up with version=" + latest + " and no data files");
    adminDB = conn.getDB("admin");
    checkFCV(adminDB, lastLTSFCV);
    MongoRunner.stopMongod(conn);
}

function runReplicaSetTest(downgradeVersion) {
    jsTestLog("Running replica set test with 'downgradeVersion': " + downgradeVersion);
    const downgradeFCV = binVersionToFCV(downgradeVersion);

    let rst;
    let rstConns;
    let replSetConfig;
    let primaryAdminDB;
    let secondaryAdminDB;

    // 'latest' binary replica set.
    rst = new ReplSetTest({nodes: 2, nodeOpts: {binVersion: latest}});
    rst.startSet();
    rst.initiate();
    primaryAdminDB = rst.getPrimary().getDB("admin");
    secondaryAdminDB = rst.getSecondary().getDB("admin");

    // FCV should default to 'latestFCV' on primary and secondary in a 'latest' binary replica set.
    checkFCV(primaryAdminDB, latestFCV);
    rst.awaitReplication();
    checkFCV(secondaryAdminDB, latestFCV);

    // featureCompatibilityVersion propagates to secondary.
    assert.commandWorked(primaryAdminDB.runCommand({setFeatureCompatibilityVersion: downgradeFCV}));
    checkFCV(primaryAdminDB, downgradeFCV);
    rst.awaitReplication();
    checkFCV(secondaryAdminDB, downgradeFCV);

    jsTestLog("EXPECTED TO FAIL: setFeatureCompatibilityVersion cannot be run on secondary");
    assert.commandFailed(secondaryAdminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));

    rst.stopSet();

    // A 'latest' binary secondary with a 'downgradeVersion' binary primary will have 'downgradeFCV'
    rst = new ReplSetTest({nodes: [{binVersion: downgradeVersion}, {binVersion: latest}]});
    rstConns = rst.startSet();
    replSetConfig = rst.getReplSetConfig();
    replSetConfig.members[1].priority = 0;
    replSetConfig.members[1].votes = 0;
    rst.initiate(replSetConfig);
    rst.waitForState(rstConns[0], ReplSetTest.State.PRIMARY);
    secondaryAdminDB = rst.getSecondary().getDB("admin");
    checkFCV(secondaryAdminDB, downgradeFCV);
    rst.stopSet();

    // Test that a 'downgradeVersion' secondary can successfully perform initial sync from a
    // 'latest' primary with 'downgradeFCV'.
    rst = new ReplSetTest({
        nodes: [{binVersion: latest}, {binVersion: latest, rsConfig: {priority: 0}}],
        settings: {chainingAllowed: false}
    });
    rst.startSet();
    rst.initiate();

    let primary = rst.getPrimary();
    primaryAdminDB = primary.getDB("admin");
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: downgradeFCV}));

    let secondary = rst.getSecondary();

    // The command should fail because wtimeout expires before a majority responds.
    stopServerReplication(secondary);
    res = primary.adminCommand(
        {setFeatureCompatibilityVersion: latestFCV, writeConcern: {wtimeout: 1000}});
    assert.eq(0, res.ok);
    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernFailed);
    restartServerReplication(secondary);

    // Downgrading the FCV should fail if a previous upgrade has not yet completed.
    assert.commandFailedWithCode(
        primary.adminCommand({setFeatureCompatibilityVersion: downgradeFCV}),
        ErrorCodes.IllegalOperation);

    if (downgradeFCV === lastLTSFCV && lastLTSFCV !== lastContinuousFCV) {
        // Upgrading to last-continuous should fail if we are in the middle of upgrading to latest.
        assert.commandFailedWithCode(
            primary.adminCommand({setFeatureCompatibilityVersion: lastContinuousFCV}), 5070602);
        assert.commandFailedWithCode(
            primary.adminCommand(
                {setFeatureCompatibilityVersion: lastContinuousFCV, fromConfigServer: true}),
            5070602);
    }

    // Because the failed upgrade command left the primary in an intermediary state, complete the
    // upgrade.
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    stopServerReplication(secondary);
    res = primary.adminCommand(
        {setFeatureCompatibilityVersion: downgradeFCV, writeConcern: {wtimeout: 1000}});
    assert.eq(0, res.ok);
    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernFailed);
    restartServerReplication(secondary);

    // Upgrading the FCV should fail if a previous downgrade has not yet completed.
    assert.commandFailedWithCode(primary.adminCommand({setFeatureCompatibilityVersion: latestFCV}),
                                 ErrorCodes.IllegalOperation);

    if (lastContinuousFCV !== lastLTSFCV) {
        // We will fail if we have not yet completed a downgrade and attempt to downgrade to a
        // different target version.
        assert.commandFailedWithCode(primary.adminCommand({
            setFeatureCompatibilityVersion: downgradeFCV === lastContinuousFCV ? lastLTSFCV
                                                                               : lastContinuousFCV
        }),
                                     ErrorCodes.IllegalOperation);
    }
    // Complete the downgrade.
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: downgradeFCV}));

    if (downgradeFCV === lastLTSFCV && lastContinuousFCV !== lastLTSFCV) {
        // The command should fail because wtimeout expires before a majority responds.
        stopServerReplication(secondary);
        res = primary.adminCommand({
            setFeatureCompatibilityVersion: lastContinuousFCV,
            fromConfigServer: true,
            writeConcern: {wtimeout: 1000}
        });
        assert.eq(0, res.ok);
        assert.commandFailedWithCode(res, ErrorCodes.WriteConcernFailed);
        restartServerReplication(secondary);

        // Upgrading the FCV to latest should fail if a previous upgrade to lastContinuous has not
        // yet completed.
        assert.commandFailedWithCode(
            primary.adminCommand({setFeatureCompatibilityVersion: latestFCV}), 5070602);

        // Complete the upgrade to last-continuous.
        assert.commandWorked(primary.adminCommand(
            {setFeatureCompatibilityVersion: lastContinuousFCV, fromConfigServer: true}));

        // Reset the FCV back to last-lts.
        assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
        assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    }

    secondary = rst.add({binVersion: downgradeVersion});
    secondaryAdminDB = secondary.getDB("admin");

    // Rig the election so that the first node running latest version remains the primary after the
    // 'downgradeVersion' secondary is added to the replica set.
    replSetConfig = rst.getReplSetConfig();
    replSetConfig.version = 4;
    replSetConfig.members[2].priority = 0;
    reconfig(rst, replSetConfig);

    // Verify that the 'downgradeVersion' secondary successfully performed its initial sync.
    assert.commandWorked(
        primaryAdminDB.getSiblingDB("test").coll.insert({awaitRepl: true}, {writeConcern: {w: 3}}));

    // Test that a downgraded secondary can no longer replicate from the primary after the FCV is
    // upgraded to 'latestFCV'.
    // Note: the downgraded secondary must stop replicating during the upgrade to ensure it has no
    // chance of seeing the 'upgrading to latest' message in the oplog, whereupon it would crash.
    stopServerReplication(secondary);
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
    restartServerReplication(secondary);
    checkFCV(secondaryAdminDB, downgradeFCV);
    assert.commandWorked(primaryAdminDB.getSiblingDB("test").coll.insert({shouldReplicate: false}));
    assert.eq(secondaryAdminDB.getSiblingDB("test").coll.find({shouldReplicate: false}).itcount(),
              0);
    rst.stopSet();

    // Test idempotency for setFeatureCompatibilityVersion.
    rst = new ReplSetTest({nodes: 2, nodeOpts: {binVersion: latest}});
    rst.startSet();
    rst.initiate();

    // Set FCV to 'downgradeFCV' so that a 'downgradeVersion' binary node can join the set.
    primary = rst.getPrimary();
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: downgradeFCV}));
    rst.awaitReplication();

    // Add a 'downgradeVersion' binary node to the set.
    secondary = rst.add({binVersion: downgradeVersion});
    rst.reInitiate();

    // Ensure the 'downgradeVersion' binary node succeeded its initial sync.
    assert.commandWorked(
        primary.getDB("test").coll.insert({awaitRepl: true}, {writeConcern: {w: 3}}));

    // Run {setFCV: downgradeFCV}. This should be idempotent.
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: downgradeFCV}));
    rst.awaitReplication();

    // Ensure the secondary is still running.
    rst.stopSet();
}

function runShardingTest(downgradeVersion) {
    jsTestLog("Running sharding test with 'downgradeVersion': " + downgradeVersion);
    const downgradeFCV = binVersionToFCV(downgradeVersion);
    let st;
    let mongosAdminDB;
    let configPrimaryAdminDB;
    let shardPrimaryAdminDB;

    // A 'latest' binary cluster started with clean data files will set FCV to 'latestFCV'.
    st = new ShardingTest({
        shards: {rs0: {nodes: [{binVersion: latest}, {binVersion: latest}]}},
        other: {useBridge: true}
    });
    mongosAdminDB = st.s.getDB("admin");
    configPrimaryAdminDB = st.configRS.getPrimary().getDB("admin");
    shardPrimaryAdminDB = st.rs0.getPrimary().getDB("admin");

    checkFCV(configPrimaryAdminDB, latestFCV);
    checkFCV(shardPrimaryAdminDB, latestFCV);

    jsTestLog(
        "EXPECTED TO FAIL: featureCompatibilityVersion cannot be set to invalid value on mongos");
    assert.commandFailed(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: 5}));
    assert.commandFailed(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: "3.2"}));
    assert.commandFailed(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: "4.8"}));

    jsTestLog("EXPECTED TO FAIL: setFeatureCompatibilityVersion rejects unknown fields on mongos");
    assert.commandFailed(
        mongosAdminDB.runCommand({setFeatureCompatibilityVersion: downgradeFCV, unknown: 1}));

    jsTestLog(
        "EXPECTED TO FAIL: setFeatureCompatibilityVersion can only be run on the admin database on mongos");
    assert.commandFailed(
        st.s.getDB("test").runCommand({setFeatureCompatibilityVersion: downgradeFCV}));

    jsTestLog(
        "EXPECTED TO FAIL: featureCompatibilityVersion cannot be set via setParameter on mongos");
    assert.commandFailed(
        mongosAdminDB.runCommand({setParameter: 1, featureCompatibilityVersion: downgradeFCV}));

    // Prevent the shard primary from receiving messages from the config server primary. When we try
    // to set FCV to 'downgradeFCV', the command should fail because the shard cannot be contacted.
    st.rs0.getPrimary().discardMessagesFrom(st.configRS.getPrimary(), 1.0);
    jsTestLog(
        "EXPECTED TO FAIL: setFeatureCompatibilityVersion cannot be set because the shard primary is not reachable");
    assert.commandFailed(
        mongosAdminDB.runCommand({setFeatureCompatibilityVersion: downgradeFCV, maxTimeMS: 1000}));
    checkFCV(
        configPrimaryAdminDB, downgradeFCV, downgradeFCV /* indicates downgrade in progress */);
    st.rs0.getPrimary().discardMessagesFrom(st.configRS.getPrimary(), 0.0);

    // FCV can be set to 'downgradeFCV' on mongos.
    // This is run through assert.soon() because we've just caused a network interruption
    // by discarding messages in the bridge.
    assert.soon(function() {
        res = mongosAdminDB.runCommand({setFeatureCompatibilityVersion: downgradeFCV});
        if (res.ok == 0) {
            print("Failed to set feature compatibility version: " + tojson(res));
            return false;
        }
        return true;
    });

    // featureCompatibilityVersion propagates to config and shard.
    checkFCV(configPrimaryAdminDB, downgradeFCV);
    checkFCV(shardPrimaryAdminDB, downgradeFCV);

    // A 'latest' binary replica set started as a shard server defaults to 'lastLTSFCV'.
    let latestShard = new ReplSetTest({
        name: "latestShard",
        nodes: [{binVersion: latest}, {binVersion: latest}],
        nodeOptions: {shardsvr: ""},
        useHostName: true
    });
    latestShard.startSet();
    latestShard.initiate();
    let latestShardPrimaryAdminDB = latestShard.getPrimary().getDB("admin");
    checkFCV(latestShardPrimaryAdminDB, lastLTSFCV);
    assert.commandWorked(mongosAdminDB.runCommand({addShard: latestShard.getURL()}));
    checkFCV(latestShardPrimaryAdminDB, downgradeFCV);

    // FCV can be set to 'latestFCV' on mongos.
    assert.commandWorked(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
    checkFCV(st.configRS.getPrimary().getDB("admin"), latestFCV);
    checkFCV(shardPrimaryAdminDB, latestFCV);
    checkFCV(latestShardPrimaryAdminDB, latestFCV);

    // Call ShardingTest.stop before shutting down latestShard, so that the UUID check in
    // ShardingTest.stop can talk to latestShard.
    st.stop();
    latestShard.stopSet();

    // Create cluster with a 'downgradeVersion' binary mongos so that we can add 'downgradeVersion'
    // binary shards.
    st = new ShardingTest({shards: 0, other: {mongosOptions: {binVersion: downgradeVersion}}});
    mongosAdminDB = st.s.getDB("admin");
    configPrimaryAdminDB = st.configRS.getPrimary().getDB("admin");
    checkFCV(configPrimaryAdminDB, downgradeFCV);

    // Adding a 'downgradeVersion' binary shard to a cluster with 'downgradeFCV' succeeds.
    let downgradedShard = new ReplSetTest({
        name: "downgradedShard",
        nodes: [{binVersion: downgradeVersion}, {binVersion: downgradeVersion}],
        nodeOptions: {shardsvr: ""},
        useHostName: true
    });
    downgradedShard.startSet();
    downgradedShard.initiate();
    assert.commandWorked(mongosAdminDB.runCommand({addShard: downgradedShard.getURL()}));
    checkFCV(downgradedShard.getPrimary().getDB("admin"), downgradeFCV);

    // call ShardingTest.stop before shutting down downgradedShard, so that the UUID check in
    // ShardingTest.stop can talk to downgradedShard.
    st.stop();
    downgradedShard.stopSet();
}

runStandaloneTest('last-lts');
runReplicaSetTest('last-lts');
runShardingTest('last-lts');

if (lastLTSFCV != lastContinuousFCV) {
    runStandaloneTest('last-continuous');
    runReplicaSetTest('last-continuous');
    runShardingTest('last-continuous');
}
})();
