
// Checking UUID consistency involves talking to a shard node, which in this test is shutdown
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

// Tests for setFeatureCompatibilityVersion.
(function() {
    "use strict";

    load("jstests/replsets/rslib.js");
    load("jstests/libs/feature_compatibility_version.js");
    load("jstests/libs/get_index_helpers.js");

    let res;
    const latest = "latest";
    const downgrade = "3.4";

    let recoverMMapJournal = function(isMMAPv1, conn, dbpath) {
        // If we're using mmapv1, recover the journal files from the unclean shutdown before
        // attempting to run with --repair.
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

    let doStartupFailTests = function(withUUIDs, dbpath) {
        // Fail to start up if no featureCompatibilityVersion document is present and non-local
        // databases are present.
        if (withUUIDs) {
            setupMissingFCVDoc(latest, dbpath);
        } else {
            setupMissingFCVDoc(downgrade, dbpath);
        }
        conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest, noCleanData: true});
        assert.eq(
            null,
            conn,
            "expected mongod to fail when data files are present and no featureCompatibilityVersion document is found.");
        if (!withUUIDs) {
            conn =
                MongoRunner.runMongod({dbpath: dbpath, binVersion: downgrade, noCleanData: true});
            assert.neq(null, conn, "expected 3.4 to startup when the FCV document is missing");
            MongoRunner.stopMongod(conn);
        }
    };

    let setupMissingFCVDoc = function(version, dbpath) {
        let conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: version});
        assert.neq(null,
                   conn,
                   "mongod was unable to start up with version=" + version + " and no data files");
        adminDB = conn.getDB("admin");
        if (version === latest) {
            assert.eq(
                adminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version,
                "3.6",
                "expected 3.6 mongod with no data files to start up with featureCompatibilityVersion 3.6");
            removeFCVDocument(adminDB);
        } else {
            assert.eq(
                adminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version,
                "3.4",
                "expected 3.4 mongod with no data files to start up with featureCompatibilityVersion 3.4");
            assert.writeOK(adminDB.system.version.remove({_id: "featureCompatibilityVersion"}));
        }

        MongoRunner.stopMongod(conn);
        return conn;
    };

    //
    // Standalone tests.
    //

    let dbpath = MongoRunner.dataPath + "feature_compatibility_version";
    resetDbpath(dbpath);
    let conn;
    let adminDB;

    // New 3.6 standalone.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest});
    assert.neq(
        null, conn, "mongod was unable to start up with version=" + latest + " and no data files");
    adminDB = conn.getDB("admin");

    // Initially featureCompatibilityVersion is 3.6.
    checkFCV(adminDB, "3.6");

    jsTestLog("EXPECTED TO FAIL: featureCompatibilityVersion cannot be set to invalid value.");
    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: 5}));
    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: "3.2"}));
    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: "3.8"}));

    jsTestLog("EXPECTED TO FAIL: setFeatureCompatibilityVersion rejects unknown fields.");
    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: "3.4", unknown: 1}));

    jsTestLog(
        "EXPECTED TO FAIL: setFeatureCompatibilityVersion can only be run on the admin database.");
    assert.commandFailed(conn.getDB("test").runCommand({setFeatureCompatibilityVersion: "3.4"}));

    jsTestLog("EXPECTED TO FAIL: featureCompatibilityVersion cannot be set via setParameter.");
    assert.commandFailed(adminDB.runCommand({setParameter: 1, featureCompatibilityVersion: "3.4"}));

    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: "failCollectionUpdates",
        data: {collectionNS: "admin.system.version"},
        mode: "alwaysOn"
    }));
    jsTestLog(
        "EXPECTED TO FAIL: setFeatureCompatibilityVersion fails to downgrade to FCV=3.4 if the write fails.");
    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: "3.4"}));
    checkFCV(adminDB, "3.6");
    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: "failCollectionUpdates",
        data: {collectionNS: "admin.system.version"},
        mode: "off"
    }));

    jsTestLog("EXPECTED TO FAIL: featureCompatibilityVersion can be set to 3.4.");
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.4"}));
    checkFCV(adminDB, "3.4");

    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: "failCollectionUpdates",
        data: {collectionNS: "admin.system.version"},
        mode: "alwaysOn"
    }));
    jsTestLog(
        "EXPECTED TO FAIL: setFeatureCompatibilityVersion fails to upgrade to FCV=3.6 if the write fails.");
    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: "3.6"}));
    checkFCV(adminDB, "3.4");
    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: "failCollectionUpdates",
        data: {collectionNS: "admin.system.version"},
        mode: "off"
    }));

    // featureCompatibilityVersion can be set to 3.6.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.6"}));
    checkFCV(adminDB, "3.6");

    MongoRunner.stopMongod(conn);

    // featureCompatibilityVersion is durable.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest});
    assert.neq(
        null, conn, "mongod was unable to start up with version=" + latest + " and no data files");
    adminDB = conn.getDB("admin");
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.4"}));
    checkFCV(adminDB, "3.4");
    MongoRunner.stopMongod(conn);

    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest, noCleanData: true});
    assert.neq(null,
               conn,
               "mongod was unable to start up with version=" + latest +
                   " and featureCompatibilityVersion=3.4");
    adminDB = conn.getDB("admin");
    checkFCV(adminDB, "3.4");
    MongoRunner.stopMongod(conn);

    // If you upgrade from 3.4 to 3.6 and have non-local databases, featureCompatibilityVersion is
    // 3.4.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: downgrade});
    assert.neq(
        null, conn, "mongod was unable to start up with version=" + latest + " and no data files");
    assert.writeOK(conn.getDB("test").coll.insert({a: 5}));
    adminDB = conn.getDB("admin");
    checkFCV34(adminDB, "3.4");
    MongoRunner.stopMongod(conn);

    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest, noCleanData: true});
    assert.neq(null,
               conn,
               "mongod was unable to start up with version=" + latest +
                   " and featureCompatibilityVersion=3.4");
    adminDB = conn.getDB("admin");
    checkFCV(adminDB, "3.4");
    MongoRunner.stopMongod(conn);

    // A 3.6 mongod started with --shardsvr and clean data files gets featureCompatibilityVersion
    // 3.4.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest, shardsvr: ""});
    assert.neq(
        null, conn, "mongod was unable to start up with version=" + latest + " and no data files");
    adminDB = conn.getDB("admin");
    checkFCV(adminDB, "3.4");
    MongoRunner.stopMongod(conn);

    const isMMAPv1 = jsTest.options().storageEngine === "mmapv1";
    // Fail to start up if no featureCompatibilityVersion is present.
    doStartupFailTests(/*withUUIDs*/ true, dbpath);

    // Fail to start up if no featureCompatibilityVersion is present and no collections have UUIDs.
    doStartupFailTests(/*withUUIDs*/ false, dbpath);

    // --repair can be used to restore a missing featureCompatibilityVersion document to an
    // existing admin database if at least some collections have UUIDs.
    conn = setupMissingFCVDoc(latest, dbpath);
    recoverMMapJournal(isMMAPv1, conn, dbpath);
    let returnCode = runMongoProgram("mongod", "--port", conn.port, "--repair", "--dbpath", dbpath);
    assert.eq(
        returnCode,
        0,
        "expected mongod --repair to execute successfully when restoring a missing featureCompatibilityVersion document.");
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest, noCleanData: true});
    assert.neq(null,
               conn,
               "mongod was unable to start up with version=" + latest + " and existing data files");
    // featureCompatibilityVersion is 3.6 because all collections were left intact with UUIDs.
    adminDB = conn.getDB("admin");
    assert.eq(adminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version, "3.6");
    MongoRunner.stopMongod(conn);

    // --repair cannot be used to restore a missing featureCompatibilityVersion document if there
    // are no collections with UUIDs.
    conn = setupMissingFCVDoc(downgrade, dbpath);
    recoverMMapJournal(isMMAPv1, conn, dbpath);
    returnCode = runMongoProgram("mongod", "--port", conn.port, "--repair", "--dbpath", dbpath);
    let exitNeedsDowngradeCode = 62;
    assert.eq(
        returnCode,
        exitNeedsDowngradeCode,
        "Expected running --repair with the latest mongod to fail because no collections have UUIDs. MongoDB 3.4 is required.");

    // If the featureCompatibilityVersion document is present but there are no collection UUIDs,
    // --repair should not attempt to restore the document and thus not fassert.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: downgrade});
    assert.neq(null,
               conn,
               "mongod was unable to start up with version=" + downgrade + " and no data files");
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

    // New 3.6 replica set.
    rst = new ReplSetTest({nodes: 2, nodeOpts: {binVersion: latest}});
    rst.startSet();
    rst.initiate();
    primaryAdminDB = rst.getPrimary().getDB("admin");
    secondaryAdminDB = rst.getSecondary().getDB("admin");

    // Initially featureCompatibilityVersion is 3.6 on primary and secondary.
    checkFCV(primaryAdminDB, "3.6");
    rst.awaitReplication();
    checkFCV(secondaryAdminDB, "3.6");

    // featureCompatibilityVersion propagates to secondary.
    assert.commandWorked(primaryAdminDB.runCommand({setFeatureCompatibilityVersion: "3.4"}));
    checkFCV(primaryAdminDB, "3.4");
    rst.awaitReplication();
    checkFCV(secondaryAdminDB, "3.4");

    jsTestLog("EXPECTED TO FAIL: setFeatureCompatibilityVersion cannot be run on secondary.");
    assert.commandFailed(secondaryAdminDB.runCommand({setFeatureCompatibilityVersion: "3.6"}));

    rst.stopSet();

    // A 3.6 secondary with a 3.4 primary will have featureCompatibilityVersion=3.4.
    rst = new ReplSetTest({nodes: [{binVersion: downgrade}, {binVersion: latest}]});
    rstConns = rst.startSet();
    replSetConfig = rst.getReplSetConfig();
    replSetConfig.members[1].priority = 0;
    replSetConfig.members[1].votes = 0;
    rst.initiate(replSetConfig);
    rst.waitForState(rstConns[0], ReplSetTest.State.PRIMARY);
    secondaryAdminDB = rst.getSecondary().getDB("admin");
    checkFCV(secondaryAdminDB, "3.4");
    rst.stopSet();

    // Test that a 3.4 secondary can successfully perform initial sync from a 3.6 primary with
    // featureCompatibilityVersion=3.4.
    // Start with 2 3.6 nodes so that when the 3.4 node added later crashes the primary doesn't
    // step down.
    rst = new ReplSetTest(
        {nodes: [{binVersion: latest}, {binVersion: latest, rsConfig: {priority: 0}}]});
    rst.startSet();
    rst.initiate();

    let primary = rst.getPrimary();
    primaryAdminDB = primary.getDB("admin");
    assert.commandWorked(primaryAdminDB.runCommand({setFeatureCompatibilityVersion: "3.4"}));
    let secondary = rst.add({binVersion: downgrade});
    secondaryAdminDB = secondary.getDB("admin");

    // Rig the election so that the first node running latest version remains the primary after the
    // 3.4 secondary is added to the replica set.
    replSetConfig = rst.getReplSetConfig();
    replSetConfig.version = 4;
    replSetConfig.members[2].priority = 0;

    // The default value for 'catchUpTimeoutMillis' on 3.6 is -1. A 3.4 secondary will refuse to
    // join a replica set with catchUpTimeoutMillis=-1.
    replSetConfig.settings = {catchUpTimeoutMillis: 2000};
    reconfig(rst, replSetConfig);

    // Verify that the 3.4 secondary successfully performed its initial sync.
    assert.writeOK(
        primaryAdminDB.getSiblingDB("test").coll.insert({awaitRepl: true}, {writeConcern: {w: 3}}));

    // Test that a 3.4 secondary can no longer replicate from the primary after the
    // featureCompatibilityVersion is set to 3.6.
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: "3.6"}));
    checkFCV34(secondaryAdminDB, "3.4");
    assert.writeOK(primaryAdminDB.getSiblingDB("test").coll.insert({shouldReplicate: false}));
    assert.eq(secondaryAdminDB.getSiblingDB("test").coll.find({shouldReplicate: false}).itcount(),
              0);

    ((original) => {
        // We skip checking dbhashes when shutting down the replica set because the 3.4 secondary
        // cannot replicate from the primary when featureCompatibilityVersion=3.6.
        TestData.skipCheckDBHashes = true;
        try {
            rst.stopSet();
        } finally {
            TestData.skipCheckDBHashes = original;
        }
    })(TestData.skipCheckDBHashes);

    // Test idempotency for setFeatureCompatibilityVersion.
    rst = new ReplSetTest({nodes: 2, nodeOpts: {binVersion: latest}});
    rst.startSet();
    rst.initiate();

    // Set FCV to 3.4 so that a 3.4 node can join the set.
    primary = rst.getPrimary();
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: downgrade}));
    rst.awaitReplication();

    // Add a 3.4 node to the set.
    secondary = rst.add({binVersion: downgrade});
    rst.reInitiate();

    // Ensure the 3.4 node succeeded its initial sync.
    assert.writeOK(primary.getDB("test").coll.insert({awaitRepl: true}, {writeConcern: {w: 3}}));

    // Run {setFCV: "3.4"}. This should be idempotent.
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: downgrade}));
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

    // New 3.6 cluster.
    st = new ShardingTest({
        shards: {rs0: {nodes: [{binVersion: latest}, {binVersion: latest}]}},
        other: {useBridge: true}
    });
    mongosAdminDB = st.s.getDB("admin");
    configPrimaryAdminDB = st.configRS.getPrimary().getDB("admin");
    shardPrimaryAdminDB = st.rs0.getPrimary().getDB("admin");

    // Initially featureCompatibilityVersion is 3.6 on config and shard.
    checkFCV(configPrimaryAdminDB, "3.6");
    checkFCV(shardPrimaryAdminDB, "3.6");

    jsTestLog(
        "EXPECTED TO FAIL: featureCompatibilityVersion cannot be set to invalid value on mongos.");
    assert.commandFailed(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: 5}));
    assert.commandFailed(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: "3.2"}));
    assert.commandFailed(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: "3.8"}));

    jsTestLog("EXPECTED TO FAIL: setFeatureCompatibilityVersion rejects unknown fields on mongos.");
    assert.commandFailed(
        mongosAdminDB.runCommand({setFeatureCompatibilityVersion: "3.4", unknown: 1}));

    jsTestLog(
        "EXPECTED TO FAIL: setFeatureCompatibilityVersion can only be run on the admin database on mongos.");
    assert.commandFailed(st.s.getDB("test").runCommand({setFeatureCompatibilityVersion: "3.4"}));

    jsTestLog(
        "EXPECTED TO FAIL: featureCompatibilityVersion cannot be set via setParameter on mongos.");
    assert.commandFailed(
        mongosAdminDB.runCommand({setParameter: 1, featureCompatibilityVersion: "3.4"}));

    // Prevent the shard primary from receiving messages from the config server primary. When we try
    // to set the featureCompatibilityVersion to "3.4", the command should fail because the shard
    // cannot be contacted.
    st.rs0.getPrimary().discardMessagesFrom(st.configRS.getPrimary(), 1.0);
    jsTestLog(
        "EXPECTED TO FAIL: setFeatureCompatibilityVersion to 3.4 should fail because the shard cannot be contacted.");
    assert.commandFailed(
        mongosAdminDB.runCommand({setFeatureCompatibilityVersion: "3.4", maxTimeMS: 1000}));
    checkFCV(configPrimaryAdminDB, "3.4", "3.4" /* indicates downgrade in progress */);
    st.rs0.getPrimary().discardMessagesFrom(st.configRS.getPrimary(), 0.0);

    // featureCompatibilityVersion can be set to 3.4 on mongos.
    // This is run through assert.soon() because we've just caused a network interruption
    // by discarding messages in the bridge.
    assert.soon(function() {
        res = mongosAdminDB.runCommand({setFeatureCompatibilityVersion: "3.4"});
        if (res.ok == 0) {
            print("Failed to set feature compatibility version: " + tojson(res));
            return false;
        }
        return true;
    });

    // featureCompatibilityVersion propagates to config and shard.
    checkFCV(configPrimaryAdminDB, "3.4");
    checkFCV(shardPrimaryAdminDB, "3.4");

    // A 3.6 shard added to a cluster with featureCompatibilityVersion=3.4 gets
    // featureCompatibilityVersion=3.4.
    let latestShard = new ReplSetTest({
        name: "latestShard",
        nodes: [{binVersion: latest}, {binVersion: latest}],
        nodeOptions: {shardsvr: ""},
        useHostName: true
    });
    latestShard.startSet();
    latestShard.initiate();
    let latestShardPrimaryAdminDB = latestShard.getPrimary().getDB("admin");
    // The featureCompatibilityVersion is 3.4 before the shard is added.
    checkFCV(latestShardPrimaryAdminDB, "3.4");
    assert.commandWorked(mongosAdminDB.runCommand({addShard: latestShard.getURL()}));
    checkFCV(latestShardPrimaryAdminDB, "3.4");

    // Shard some collections before upgrade, to ensure UUIDs are generated for them on upgrade.
    // This specifically tests 3.4 -> 3.6 upgrade behavior.
    let dbName = "test";
    assert.commandWorked(mongosAdminDB.runCommand({enableSharding: dbName}));
    let existingShardedCollNames = [];
    for (let i = 0; i < 5; i++) {
        let collName = "coll" + i;
        assert.commandWorked(
            mongosAdminDB.runCommand({shardCollection: dbName + "." + collName, key: {_id: 1}}));
        existingShardedCollNames.push(collName);
    }

    // Additionally simulate that some sharded collections already have UUIDs from a previous failed
    // upgrade attempt (for example, due to repeated config server failover).
    let existingShardedCollEntriesWithUUIDs = [];
    for (let i = 0; i < 3; i++) {
        let collName = "collWithUUID" + i;
        let collEntry = {
            _id: dbName + "." + collName,
            lastmod: ISODate(),
            dropped: false,
            key: {_id: 1},
            unique: false,
            lastmodEpoch: ObjectId(),
            uuid: UUID()
        };
        assert.writeOK(st.s.getDB("config").collections.insert(collEntry));
        existingShardedCollEntriesWithUUIDs.push(collEntry);
    }

    // featureCompatibilityVersion can be set to 3.6 on mongos.
    assert.commandWorked(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: "3.6"}));
    checkFCV(st.configRS.getPrimary().getDB("admin"), "3.6");
    checkFCV(shardPrimaryAdminDB, "3.6");

    // Ensure the storage engine's schema was upgraded on the config server to include UUIDs.
    // This specifically tests 3.4 -> 3.6 upgrade behavior.
    res = st.s.getDB("config").runCommand({listCollections: 1});
    assert.commandWorked(res);
    for (let coll of res.cursor.firstBatch) {
        assert(coll.info.hasOwnProperty("uuid"), tojson(res));
    }

    // Check that the existing sharded collections that did not have UUIDs were assigned new UUIDs,
    // and existing sharded collections that already had a UUID (from the simulated earlier failed
    // upgrade attempt) were *not* assigned new UUIDs.
    // This specifically tests 3.4 -> 3.6 upgrade behavior.
    existingShardedCollNames.forEach(function(collName) {
        let collEntry = st.s.getDB("config").collections.findOne({_id: dbName + "." + collName});
        assert.neq(null, collEntry);
        assert.hasFields(collEntry, ["uuid"]);
    });
    existingShardedCollEntriesWithUUIDs.forEach(function(expectedEntry) {
        let actualEntry = st.s.getDB("config").collections.findOne({_id: expectedEntry._id});
        assert.neq(null, actualEntry);
        assert.docEq(expectedEntry, actualEntry);
    });

    // Call ShardingTest.stop before shutting down latestShard, so that the UUID check in
    // ShardingTest.stop can talk to latestShard.
    st.stop();
    latestShard.stopSet();

    // Create cluster with 3.4 mongos so that we can add 3.4 shards.
    st = new ShardingTest({shards: 0, other: {mongosOptions: {binVersion: downgrade}}});
    mongosAdminDB = st.s.getDB("admin");
    configPrimaryAdminDB = st.configRS.getPrimary().getDB("admin");
    checkFCV(configPrimaryAdminDB, "3.4");

    // Adding a 3.4 shard to a cluster with featureCompatibilityVersion=3.4 succeeds.
    let downgradeShard = new ReplSetTest({
        name: "downgradeShard",
        nodes: [{binVersion: downgrade}, {binVersion: downgrade}],
        nodeOptions: {shardsvr: ""},
        useHostName: true
    });
    downgradeShard.startSet();
    downgradeShard.initiate();
    assert.commandWorked(mongosAdminDB.runCommand({addShard: downgradeShard.getURL()}));
    checkFCV34(downgradeShard.getPrimary().getDB("admin"), "3.4");

    // call ShardingTest.stop before shutting down downgradeShard, so that the UUID check in
    // ShardingTest.stop can talk to downgradeShard.
    st.stop();
    downgradeShard.stopSet();

    // Create a cluster running with featureCompatibilityVersion=3.6.
    st = new ShardingTest({shards: 1, mongos: 1});
    mongosAdminDB = st.s.getDB("admin");
    configPrimaryAdminDB = st.configRS.getPrimary().getDB("admin");
    checkFCV(configPrimaryAdminDB, "3.6");
    shardPrimaryAdminDB = st.shard0.getDB("admin");
    checkFCV(shardPrimaryAdminDB, "3.6");

    // Ensure that a 3.4 mongos can be added to a featureCompatibilityVersion=3.4 cluster.

    assert.commandWorked(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: "3.4"}));
    checkFCV(configPrimaryAdminDB, "3.4");
    checkFCV(shardPrimaryAdminDB, "3.4");

    // Ensure the storage engine's schema was downgraded on the config server to remove UUIDs.
    // This specifically tests 3.4 -> 3.6 downgrade behavior.
    res = st.s.getDB("config").runCommand({listCollections: 1});
    assert.commandWorked(res);
    for (let coll of res.cursor.firstBatch) {
        assert(!coll.info.hasOwnProperty("uuid"), tojson(res));
    }

    st.configRS.awaitReplication();

    let downgradeMongos =
        MongoRunner.runMongos({configdb: st.configRS.getURL(), binVersion: downgrade});
    assert.neq(null,
               downgradeMongos,
               "mongos was unable to start up with version=" + latest +
                   " and connect to featureCompatibilityVersion=3.4 cluster");

    // Ensure that the 3.4 mongos can perform reads and writes to the shards in the cluster.
    assert.writeOK(downgradeMongos.getDB("test").foo.insert({x: 1}));
    let foundDoc = downgradeMongos.getDB("test").foo.findOne({x: 1});
    assert.neq(null, foundDoc);
    assert.eq(1, foundDoc.x, tojson(foundDoc));

    // The 3.4 mongos can no longer perform reads and writes after the featureCompatibilityVersion
    // is set to 3.6.
    assert.commandWorked(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: "3.6"}));
    assert.writeError(downgradeMongos.getDB("test").foo.insert({x: 1}));

    // The 3.6 mongos can still perform reads and writes after the featureCompatibilityVersion is
    // set to 3.6.
    assert.writeOK(st.s.getDB("test").foo.insert({x: 1}));

    st.stop();
    MongoRunner.stopMongos(downgradeMongos);
})();
