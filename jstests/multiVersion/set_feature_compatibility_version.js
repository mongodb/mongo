/**
 * Tests setFeatureCompatibilityVersion.
 */

// Checking UUID consistency involves talking to a shard node, which in this test is shutdown
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

// Some of the test cases in this file clear the FCV document, so the validate command's UUID checks
// will fail.
TestData.skipCollectionAndIndexValidation = true;

// A replset test case checks that replication to a secondary ceases, so we do not expect identical
// data.
TestData.skipCheckDBHashes = true;

(function() {
    "use strict";

    load("jstests/replsets/rslib.js");
    load("jstests/libs/feature_compatibility_version.js");
    load("jstests/libs/get_index_helpers.js");

    let dbpath = MongoRunner.dataPath + "feature_compatibility_version";
    resetDbpath(dbpath);
    let res;

    const latest = "latest";
    const lastStable = "last-stable";
    const latestFCV = "4.0";
    const lastStableFCV = "3.6";

    /**
     * If we're using mmapv1, we must recover the journal files from an unclean shutdown before
     * attempting to run with --repair.
     */
    let recoverMMapJournal = function(isMMAPv1, conn, dbpath) {
        if (isMMAPv1) {
            let returnCode = runMongoProgram("mongod",
                                             "--port",
                                             conn.port,
                                             "--journalOptions",
                                             /*MMAPV1Options::JournalRecoverOnly*/ 4,
                                             "--dbpath",
                                             dbpath);
            assert.eq(returnCode, /*EXIT_NET_ERROR*/ 48);
        }
    };

    /**
     * Ensure that a mongod (without using --repair) fails to start up if there are non-local
     * collections and either the admin database has been dropped or the FCV document in the admin
     * database has been removed.
     *
     * The mongod has 'version' binary and is started up on 'dbpath'.
     */
    let doStartupFailTests = function(version, dbpath) {
        // Set up a mongod with an admin database but without a FCV document in the admin database.
        setupMissingFCVDoc(version, dbpath);

        // Now attempt to start up a new mongod without clearing the data files from 'dbpath', which
        // contain the admin database but are missing the FCV document. The mongod should fail to
        // start start up if there is a non-local collection and the FCV document is missing.
        conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: version, noCleanData: true});
        assert.eq(
            null,
            conn,
            "expected mongod to fail when data files are present but no FCV document is found.");
    };

    /**
     * Starts up a mongod with binary 'version' on 'dbpath', then removes the FCV document from the
     * admin database and returns the mongod.
     */
    let setupMissingFCVDoc = function(version, dbpath) {
        let conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: version});
        assert.neq(null,
                   conn,
                   "mongod was unable to start up with version=" + version + " and no data files");
        adminDB = conn.getDB("admin");
        removeFCVDocument(adminDB);
        MongoRunner.stopMongod(conn);
        return conn;
    };

    //
    // Standalone tests.
    //

    let conn;
    let adminDB;

    // A 'latest' binary standalone should default to 'latestFCV'.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest});
    assert.neq(
        null, conn, "mongod was unable to start up with version=" + latest + " and no data files");
    adminDB = conn.getDB("admin");
    checkFCV(adminDB,
             lastStableFCV);  // TODO: make this 'latestFCV' when SERVER-32597 bumps default

    // TODO: remove this when SERVER-32597 bumps the default to 4.0.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));

    // featureCompatibilityVersion cannot be set to invalid value.
    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: 5}));
    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: "3.2"}));
    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: "4.2"}));
    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: "3.4"}));

    // setFeatureCompatibilityVersion rejects unknown fields.
    assert.commandFailed(
        adminDB.runCommand({setFeatureCompatibilityVersion: lastStable, unknown: 1}));

    // setFeatureCompatibilityVersion can only be run on the admin database.
    assert.commandFailed(
        conn.getDB("test").runCommand({setFeatureCompatibilityVersion: lastStable}));

    // featureCompatibilityVersion cannot be set via setParameter.
    assert.commandFailed(
        adminDB.runCommand({setParameter: 1, featureCompatibilityVersion: lastStable}));

    // setFeatureCompatibilityVersion fails to downgrade FCV if the write fails.
    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: "failCollectionUpdates",
        data: {collectionNS: "admin.system.version"},
        mode: "alwaysOn"
    }));
    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: lastStableFCV}));
    checkFCV(adminDB, latestFCV);
    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: "failCollectionUpdates",
        data: {collectionNS: "admin.system.version"},
        mode: "off"
    }));

    // featureCompatibilityVersion can be downgraded to 'lastStableFCV'.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: lastStableFCV}));
    checkFCV(adminDB, lastStableFCV);

    // setFeatureCompatibilityVersion fails to upgrade to 'latestFCV' if the write fails.
    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: "failCollectionUpdates",
        data: {collectionNS: "admin.system.version"},
        mode: "alwaysOn"
    }));
    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
    checkFCV(adminDB, lastStableFCV);
    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: "failCollectionUpdates",
        data: {collectionNS: "admin.system.version"},
        mode: "off"
    }));

    // featureCompatibilityVersion can be upgraded to 'latestFCV'.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
    checkFCV(adminDB, latestFCV);

    // TODO: remove these FCV 3.4 / 4.0 tests when FCV 3.4 is removed (SERVER-32597).
    // ----------------------------------------------------------------------------------

    // Cannot jump down two FCVs from 4.0 to 3.4
    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: "3.4"}));
    checkFCV(adminDB, latestFCV);

    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: lastStableFCV}));
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.4"}));
    checkFCV(adminDB, "3.4");

    // Cannot jump up two FCVs from 3.4 to 4.0
    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
    checkFCV(adminDB, "3.4");

    // ------------------ end FCV 3.4/4.0 testing ---------------------------------------

    MongoRunner.stopMongod(conn);

    // featureCompatibilityVersion is durable.
    // TODO: update this to use 'latestFCV' and 'lastStableFCV', rather than 'lastStableFCV' and
    // '3.4', respectively, when SERVER-32597 bumps the default to 4.0.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest});
    assert.neq(
        null, conn, "mongod was unable to start up with version=" + latest + " and no data files");
    adminDB = conn.getDB("admin");
    checkFCV(adminDB, lastStableFCV);
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.4"}));
    checkFCV(adminDB, "3.4");
    MongoRunner.stopMongod(conn);

    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest, noCleanData: true});
    assert.neq(null,
               conn,
               "mongod was unable to start up with binary version=" + latest +
                   " and featureCompatibilityVersion=3.4");
    adminDB = conn.getDB("admin");
    checkFCV(adminDB, "3.4");
    MongoRunner.stopMongod(conn);

    // If you upgrade from 'lastStable' binary to 'latest' binary and have non-local databases, FCV
    // remains 'lastStableFCV'.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: lastStable});
    assert.neq(null,
               conn,
               "mongod was unable to start up with version=" + lastStable + " and no data files");
    assert.writeOK(conn.getDB("test").coll.insert({a: 5}));
    adminDB = conn.getDB("admin");
    checkFCV(adminDB, lastStableFCV);
    MongoRunner.stopMongod(conn);

    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest, noCleanData: true});
    assert.neq(null,
               conn,
               "mongod was unable to start up with binary version=" + latest +
                   " and featureCompatibilityVersion=" + lastStableFCV);
    adminDB = conn.getDB("admin");
    checkFCV(adminDB, lastStableFCV);
    MongoRunner.stopMongod(conn);

    // A 'latest' binary mongod started with --shardsvr and clean data files defaults to
    // 'lastStableFCV'.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest, shardsvr: ""});
    assert.neq(
        null, conn, "mongod was unable to start up with version=" + latest + " and no data files");
    adminDB = conn.getDB("admin");
    checkFCV(adminDB, lastStableFCV);
    MongoRunner.stopMongod(conn);

    // Check that start up without --repair fails if there is non-local DB data and either the admin
    // database was dropped or the FCV doc was deleted.
    doStartupFailTests(latest, dbpath);

    const isMMAPv1 = jsTest.options().storageEngine === "mmapv1";

    // --repair can be used to restore a missing featureCompatibilityVersion document to an existing
    // admin database if at least some collections have UUIDs.
    conn = setupMissingFCVDoc(latest, dbpath);
    recoverMMapJournal(isMMAPv1, conn, dbpath);
    let returnCode = runMongoProgram("mongod", "--port", conn.port, "--repair", "--dbpath", dbpath);
    assert.eq(
        returnCode,
        0,
        "expected mongod --repair to execute successfully when restoring a missing FCV document.");
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest, noCleanData: true});
    assert.neq(null,
               conn,
               "mongod was unable to start up with version=" + latest + " and existing data files");
    // FCV is 'latestFCV' because all collections were left intact with UUIDs.
    // TODO(SERVER-32909): update this section to reflect new FCV restoration semantics.
    adminDB = conn.getDB("admin");
    assert.eq(adminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version,
              lastStableFCV);
    MongoRunner.stopMongod(conn);

    // If the featureCompatibilityVersion document is present but there are no collection UUIDs,
    // --repair should not attempt to restore the document and should return success.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: lastStable});
    assert.neq(null,
               conn,
               "mongod was unable to start up with version=" + lastStable + " and no data files");
    MongoRunner.stopMongod(conn);
    recoverMMapJournal(isMMAPv1, conn, dbpath);
    returnCode = runMongoProgram("mongod", "--port", conn.port, "--repair", "--dbpath", dbpath);
    assert.eq(returnCode, 0);

    //
    // Replica set tests.
    //

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
    // TODO: update these to 'latestFCV' when SERVER-32597 bumps the FCV default.
    checkFCV(primaryAdminDB, lastStableFCV);
    rst.awaitReplication();
    checkFCV(secondaryAdminDB, lastStableFCV);

    // featureCompatibilityVersion propagates to secondary.
    // TODO: update these to 'lastStableFCV' when SERVER-32597 bumps the FCV default.
    assert.commandWorked(primaryAdminDB.runCommand({setFeatureCompatibilityVersion: "3.4"}));
    checkFCV(primaryAdminDB, "3.4");
    rst.awaitReplication();
    checkFCV(secondaryAdminDB, "3.4");

    // setFeatureCompatibilityVersion cannot be run on secondary.
    // TODO: update this to 'latestFCV' when SERVER-32597 bumps the FCV default.
    assert.commandFailed(
        secondaryAdminDB.runCommand({setFeatureCompatibilityVersion: lastStableFCV}));

    rst.stopSet();

    // A 'latest' binary secondary with a 'lastStable' binary primary will have 'lastStableFCV'
    rst = new ReplSetTest({nodes: [{binVersion: lastStable}, {binVersion: latest}]});
    rstConns = rst.startSet();
    replSetConfig = rst.getReplSetConfig();
    replSetConfig.members[1].priority = 0;
    replSetConfig.members[1].votes = 0;
    rst.initiate(replSetConfig);
    rst.waitForState(rstConns[0], ReplSetTest.State.PRIMARY);
    secondaryAdminDB = rst.getSecondary().getDB("admin");
    checkFCV(secondaryAdminDB, lastStableFCV);
    rst.stopSet();

    // Test that a 'lastStable' secondary can successfully perform initial sync from a 'latest'
    // primary with 'lastStableFCV'.
    rst = new ReplSetTest(
        {nodes: [{binVersion: latest}, {binVersion: latest, rsConfig: {priority: 0}}]});
    rst.startSet();
    rst.initiate();

    let primary = rst.getPrimary();
    primaryAdminDB = primary.getDB("admin");
    let secondary = rst.add({binVersion: lastStable});
    secondaryAdminDB = secondary.getDB("admin");

    // Rig the election so that the first node running latest version remains the primary after the
    // 'lastStable' secondary is added to the replica set.
    replSetConfig = rst.getReplSetConfig();
    replSetConfig.version = 4;
    replSetConfig.members[2].priority = 0;
    reconfig(rst, replSetConfig);

    // Verify that the 'lastStable' secondary successfully performed its initial sync.
    assert.writeOK(
        primaryAdminDB.getSiblingDB("test").coll.insert({awaitRepl: true}, {writeConcern: {w: 3}}));

    // Test that a 'lastStable' secondary can no longer replicate from the primary after the FCV is
    // upgraded to 'latestFCV'.
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
    checkFCV(secondaryAdminDB, lastStableFCV);
    assert.writeOK(primaryAdminDB.getSiblingDB("test").coll.insert({shouldReplicate: false}));
    assert.eq(secondaryAdminDB.getSiblingDB("test").coll.find({shouldReplicate: false}).itcount(),
              0);
    rst.stopSet();

    // Test idempotency for setFeatureCompatibilityVersion.
    rst = new ReplSetTest({nodes: 2, nodeOpts: {binVersion: latest}});
    rst.startSet();
    rst.initiate();

    // Set FCV to 'lastStableFCV' so that a 'lastStable' binary node can join the set.
    primary = rst.getPrimary();
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
    rst.awaitReplication();

    // Add a 'lastStable' binary node to the set.
    secondary = rst.add({binVersion: lastStable});
    rst.reInitiate();

    // Ensure the 'lastStable' binary node succeeded its initial sync.
    assert.writeOK(primary.getDB("test").coll.insert({awaitRepl: true}, {writeConcern: {w: 3}}));

    // Run {setFCV: lastStableFCV}. This should be idempotent.
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
    rst.awaitReplication();

    // Ensure the secondary is still running.
    rst.stopSet();

    //
    // Sharding tests.
    //

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

    // TODO: update this to 'latestFCV' when SERVER-32597 bumps the FCV default.
    checkFCV(configPrimaryAdminDB, lastStableFCV);
    checkFCV(shardPrimaryAdminDB, lastStableFCV);

    // TODO: remove this when SERVER-32597 bumps the FCV default ot 4.0.
    assert.commandWorked(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));

    // featureCompatibilityVersion cannot be set to invalid value on mongos.
    assert.commandFailed(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: 5}));
    assert.commandFailed(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: "3.2"}));
    assert.commandFailed(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: "4.2"}));

    // setFeatureCompatibilityVersion rejects unknown fields on mongos.
    assert.commandFailed(
        mongosAdminDB.runCommand({setFeatureCompatibilityVersion: lastStableFCV, unknown: 1}));

    // setFeatureCompatibilityVersion can only be run on the admin database on mongos.
    assert.commandFailed(
        st.s.getDB("test").runCommand({setFeatureCompatibilityVersion: lastStableFCV}));

    // featureCompatibilityVersion cannot be set via setParameter on mongos.
    assert.commandFailed(
        mongosAdminDB.runCommand({setParameter: 1, featureCompatibilityVersion: lastStableFCV}));

    // Prevent the shard primary from receiving messages from the config server primary. When we try
    // to set FCV to 'lastStableFCV', the command should fail because the shard cannot be contacted.
    st.rs0.getPrimary().discardMessagesFrom(st.configRS.getPrimary(), 1.0);
    assert.commandFailed(
        mongosAdminDB.runCommand({setFeatureCompatibilityVersion: lastStableFCV, maxTimeMS: 1000}));
    checkFCV(
        configPrimaryAdminDB, lastStableFCV, lastStableFCV /* indicates downgrade in progress */);
    st.rs0.getPrimary().discardMessagesFrom(st.configRS.getPrimary(), 0.0);

    // FCV can be set to 'lastStableFCV' on mongos.
    // This is run through assert.soon() because we've just caused a network interruption
    // by discarding messages in the bridge.
    assert.soon(function() {
        res = mongosAdminDB.runCommand({setFeatureCompatibilityVersion: lastStableFCV});
        if (res.ok == 0) {
            print("Failed to set feature compatibility version: " + tojson(res));
            return false;
        }
        return true;
    });

    // featureCompatibilityVersion propagates to config and shard.
    checkFCV(configPrimaryAdminDB, lastStableFCV);
    checkFCV(shardPrimaryAdminDB, lastStableFCV);

    // A 'latest' binary replica set started as a shard server defaults to 'lastStableFCV'.
    let latestShard = new ReplSetTest({
        name: "latestShard",
        nodes: [{binVersion: latest}, {binVersion: latest}],
        nodeOptions: {shardsvr: ""},
        useHostName: true
    });
    latestShard.startSet();
    latestShard.initiate();
    let latestShardPrimaryAdminDB = latestShard.getPrimary().getDB("admin");
    checkFCV(latestShardPrimaryAdminDB, lastStableFCV);
    assert.commandWorked(mongosAdminDB.runCommand({addShard: latestShard.getURL()}));
    checkFCV(latestShardPrimaryAdminDB, lastStableFCV);

    // FCV can be set to 'latestFCV' on mongos.
    assert.commandWorked(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
    checkFCV(st.configRS.getPrimary().getDB("admin"), latestFCV);
    checkFCV(shardPrimaryAdminDB, latestFCV);
    checkFCV(latestShardPrimaryAdminDB, latestFCV);

    // Call ShardingTest.stop before shutting down latestShard, so that the UUID check in
    // ShardingTest.stop can talk to latestShard.
    st.stop();
    latestShard.stopSet();

    // Create cluster with a 'lastStable' binary mongos so that we can add 'lastStable' binary
    // shards.
    st = new ShardingTest({shards: 0, other: {mongosOptions: {binVersion: lastStable}}});
    mongosAdminDB = st.s.getDB("admin");
    configPrimaryAdminDB = st.configRS.getPrimary().getDB("admin");
    checkFCV(configPrimaryAdminDB, lastStableFCV);

    // Adding a 'lastStable' binary shard to a cluster with 'lastStableFCV' succeeds.
    let lastStableShard = new ReplSetTest({
        name: "lastStableShard",
        nodes: [{binVersion: lastStable}, {binVersion: lastStable}],
        nodeOptions: {shardsvr: ""},
        useHostName: true
    });
    lastStableShard.startSet();
    lastStableShard.initiate();
    assert.commandWorked(mongosAdminDB.runCommand({addShard: lastStableShard.getURL()}));
    checkFCV(lastStableShard.getPrimary().getDB("admin"), lastStableFCV);

    // call ShardingTest.stop before shutting down lastStableShard, so that the UUID check in
    // ShardingTest.stop can talk to lastStableShard.
    st.stop();
    lastStableShard.stopSet();

    // Create a cluster running with 'latestFCV'
    st = new ShardingTest({shards: 1, mongos: 1});
    mongosAdminDB = st.s.getDB("admin");
    configPrimaryAdminDB = st.configRS.getPrimary().getDB("admin");
    shardPrimaryAdminDB = st.shard0.getDB("admin");
    // TODO: update this to 'latestFCV' when SERVER-32597 updates the FCV defaults.
    checkFCV(configPrimaryAdminDB, lastStableFCV);
    checkFCV(shardPrimaryAdminDB, lastStableFCV);

    // TODO: remove this when SERVER-32597 updates the FCV defaults.
    assert.commandWorked(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));

    // Ensure that a 'lastStable' binary mongos can be added to a 'lastStableFCV' cluster.

    assert.commandWorked(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: lastStableFCV}));
    checkFCV(configPrimaryAdminDB, lastStableFCV);
    checkFCV(shardPrimaryAdminDB, lastStableFCV);

    let lastStableMongos =
        MongoRunner.runMongos({configdb: st.configRS.getURL(), binVersion: lastStable});
    assert.neq(null,
               lastStableMongos,
               "mongos was unable to start up with binary version=" + latest +
                   " and connect to FCV=" + lastStableFCV + " cluster");

    // Ensure that the 'lastStable' binary mongos can perform reads and writes to the shards in the
    // cluster.
    assert.writeOK(lastStableMongos.getDB("test").foo.insert({x: 1}));
    let foundDoc = lastStableMongos.getDB("test").foo.findOne({x: 1});
    assert.neq(null, foundDoc);
    assert.eq(1, foundDoc.x, tojson(foundDoc));

    // The 'lastStable' binary mongos can no longer perform reads and writes after the cluster is
    // upgraded to 'latestFCV'.
    assert.commandWorked(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
    assert.writeError(lastStableMongos.getDB("test").foo.insert({x: 1}));

    // The 'latest' binary mongos can still perform reads and writes after the FCV is upgraded to
    // 'latestFCV'.
    assert.writeOK(st.s.getDB("test").foo.insert({x: 1}));

    st.stop();
})();
