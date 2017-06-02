// Tests for setFeatureCompatibilityVersion.
(function() {
    "use strict";

    load("jstests/replsets/rslib.js");
    load("jstests/libs/get_index_helpers.js");

    let res;
    const latest = "latest";
    const downgrade = "3.4";

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
    res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.6");
    assert.eq(adminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version, "3.6");

    // featureCompatibilityVersion cannot be set to invalid value.
    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: 5}));
    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: "3.2"}));
    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: "3.8"}));

    // setFeatureCompatibilityVersion rejects unknown fields.
    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: "3.4", unknown: 1}));

    // setFeatureCompatibilityVersion can only be run on the admin database.
    assert.commandFailed(conn.getDB("test").runCommand({setFeatureCompatibilityVersion: "3.4"}));

    // featureCompatibilityVersion cannot be set via setParameter.
    assert.commandFailed(adminDB.runCommand({setParameter: 1, featureCompatibilityVersion: "3.4"}));

    // featureCompatibilityVersion can be set to 3.4.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.4"}));
    res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.4");
    assert.eq(adminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version, "3.4");

    // featureCompatibilityVersion can be set to 3.6.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.6"}));
    res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.6");
    assert.eq(adminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version, "3.6");

    MongoRunner.stopMongod(conn);

    // featureCompatibilityVersion is durable.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest});
    assert.neq(
        null, conn, "mongod was unable to start up with version=" + latest + " and no data files");
    adminDB = conn.getDB("admin");
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.4"}));
    MongoRunner.stopMongod(conn);

    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest, noCleanData: true});
    assert.neq(null,
               conn,
               "mongod was unable to start up with version=" + latest +
                   " and featureCompatibilityVersion=3.4");
    adminDB = conn.getDB("admin");
    res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.4");
    assert.eq(adminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version, "3.4");
    MongoRunner.stopMongod(conn);

    // If you upgrade from 3.4 to 3.6 and have non-local databases, featureCompatibilityVersion is
    // 3.4.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: downgrade});
    assert.neq(
        null, conn, "mongod was unable to start up with version=" + latest + " and no data files");
    assert.writeOK(conn.getDB("test").coll.insert({a: 5}));
    adminDB = conn.getDB("admin");
    res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.4");
    assert.eq(adminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version, "3.4");
    MongoRunner.stopMongod(conn);

    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest, noCleanData: true});
    assert.neq(null,
               conn,
               "mongod was unable to start up with version=" + latest +
                   " and featureCompatibilityVersion=3.4");
    adminDB = conn.getDB("admin");
    res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.4");
    MongoRunner.stopMongod(conn);

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
    res = primaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.6");
    assert.eq(primaryAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version,
              "3.6");
    rst.awaitReplication();
    res = secondaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.6");
    assert.eq(secondaryAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version,
              "3.6");

    // featureCompatibilityVersion propagates to secondary.
    assert.commandWorked(primaryAdminDB.runCommand({setFeatureCompatibilityVersion: "3.4"}));
    res = primaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.4");
    assert.eq(primaryAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version,
              "3.4");
    rst.awaitReplication();
    res = secondaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.4");
    assert.eq(secondaryAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version,
              "3.4");

    // setFeatureCompatibilityVersion cannot be run on secondary.
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
    res = secondaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.4");
    rst.stopSet();

    // Test that a 3.4 secondary can successfully perform initial sync from a 3.6 primary with
    // featureCompatibilityVersion=3.4.
    rst = new ReplSetTest({nodes: [{binVersion: latest}]});
    rst.startSet();
    rst.initiate();

    primaryAdminDB = rst.getPrimary().getDB("admin");
    assert.commandWorked(primaryAdminDB.runCommand({setFeatureCompatibilityVersion: "3.4"}));
    let secondary = rst.add({binVersion: downgrade});
    secondaryAdminDB = secondary.getDB("admin");

    // Rig the election so that the first node running latest version remains the primary after the
    // 3.4 secondary is added to the replica set.
    replSetConfig = rst.getReplSetConfig();
    replSetConfig.version = 3;
    replSetConfig.members[1].priority = 0;
    replSetConfig.members[1].votes = 0;

    // The default value for 'catchUpTimeoutMillis' on 3.6 is -1. A 3.4 secondary will refuse to
    // join a replica set with catchUpTimeoutMillis=-1.
    replSetConfig.settings = {catchUpTimeoutMillis: 2000};
    reconfig(rst, replSetConfig);

    // Verify that the 3.4 secondary successfully performed its initial sync.
    assert.writeOK(
        primaryAdminDB.getSiblingDB("test").coll.insert({awaitRepl: true}, {writeConcern: {w: 2}}));

    // Test that a 3.4 secondary crashes when syncing from a 3.6 primary and the
    // featureCompatibilityVersion is set to 3.6.
    assert.commandWorked(primaryAdminDB.runCommand({setFeatureCompatibilityVersion: "3.6"}));
    rst.stop(secondary, undefined, {allowedExitCode: MongoRunner.EXIT_ABRUPT});
    rst.stopSet();

    // A mixed 3.4/3.6 replica set without a featureCompatibilityVersion document unfortunately
    // reports mixed 3.2/3.4 featureCompatibilityVersion.
    rst = new ReplSetTest({nodes: [{binVersion: downgrade}, {binVersion: latest}]});
    rstConns = rst.startSet();
    replSetConfig = rst.getReplSetConfig();
    replSetConfig.members[1].priority = 0;
    replSetConfig.members[1].votes = 0;
    rst.initiate(replSetConfig);
    primaryAdminDB = rst.getPrimary().getDB("admin");
    secondaryAdminDB = rst.getSecondary().getDB("admin");
    assert.writeOK(primaryAdminDB.system.version.remove({_id: "featureCompatibilityVersion"},
                                                        {writeConcern: {w: 2}}));
    res = primaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.2");
    res = secondaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.4");
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
    res = configPrimaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.6");
    assert.eq(
        configPrimaryAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version,
        "3.6");
    res = shardPrimaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.6");
    assert.eq(
        shardPrimaryAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version,
        "3.6");

    // featureCompatibilityVersion cannot be set to invalid value on mongos.
    assert.commandFailed(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: 5}));
    assert.commandFailed(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: "3.2"}));
    assert.commandFailed(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: "3.8"}));

    // setFeatureCompatibilityVersion rejects unknown fields on mongos.
    assert.commandFailed(
        mongosAdminDB.runCommand({setFeatureCompatibilityVersion: "3.4", unknown: 1}));

    // setFeatureCompatibilityVersion can only be run on the admin database on mongos.
    assert.commandFailed(st.s.getDB("test").runCommand({setFeatureCompatibilityVersion: "3.4"}));

    // featureCompatibilityVersion cannot be set via setParameter on mongos.
    assert.commandFailed(
        mongosAdminDB.runCommand({setParameter: 1, featureCompatibilityVersion: "3.4"}));

    // Prevent the shard primary from receiving messages from the config server primary. When we try
    // to set the featureCompatibilityVersion to "3.4", it should fail because the shard cannot be
    // contacted. The config server primary should still report "3.6", since setting the version to
    // "3.4" did not succeed.
    st.rs0.getPrimary().discardMessagesFrom(st.configRS.getPrimary(), 1.0);
    assert.commandFailed(
        mongosAdminDB.runCommand({setFeatureCompatibilityVersion: "3.4", maxTimeMS: 1000}));
    res = configPrimaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.6");
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
    res = configPrimaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.4");
    assert.eq(
        configPrimaryAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version,
        "3.4");
    res = shardPrimaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.4");
    assert.eq(
        shardPrimaryAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version,
        "3.4");

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
    assert.commandWorked(mongosAdminDB.runCommand({addShard: latestShard.getURL()}));
    let latestShardPrimaryAdminDB = latestShard.getPrimary().getDB("admin");
    res = latestShardPrimaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.4");

    // featureCompatibilityVersion can be set to 3.6 on mongos.
    assert.commandWorked(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: "3.6"}));
    res = st.configRS.getPrimary().getDB("admin").runCommand(
        {getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.6");
    assert.eq(
        configPrimaryAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version,
        "3.6");
    res = shardPrimaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.6");
    assert.eq(
        shardPrimaryAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version,
        "3.6");

    latestShard.stopSet();
    st.stop();

    // Create cluster with 3.4 mongos so that we can add 3.4 shards.
    st = new ShardingTest({shards: 0, other: {mongosOptions: {binVersion: downgrade}}});
    mongosAdminDB = st.s.getDB("admin");
    configPrimaryAdminDB = st.configRS.getPrimary().getDB("admin");

    // Adding a 3.4 shard to a cluster with featureCompatibilityVersion=3.4 succeeds.
    res = configPrimaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.4");
    assert.eq(
        configPrimaryAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version,
        "3.4");
    let downgradeShard = new ReplSetTest({
        name: "downgradeShard",
        nodes: [{binVersion: downgrade}, {binVersion: downgrade}],
        nodeOptions: {shardsvr: ""},
        useHostName: true
    });
    downgradeShard.startSet();
    downgradeShard.initiate();
    assert.commandWorked(mongosAdminDB.runCommand({addShard: downgradeShard.getURL()}));
    res =
        downgradeShard.getPrimary().adminCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.4");

    downgradeShard.stopSet();
    st.stop();

    // Create a cluster running with featureCompatibilityVersion=3.6.
    st = new ShardingTest({shards: 1, mongos: 1});
    mongosAdminDB = st.s.getDB("admin");
    configPrimaryAdminDB = st.configRS.getPrimary().getDB("admin");
    shardPrimaryAdminDB = st.shard0.getDB("admin");
    res = configPrimaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.6");
    assert.eq(
        configPrimaryAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version,
        "3.6");
    res = shardPrimaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.6");
    assert.eq(
        shardPrimaryAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version,
        "3.6");

    // Ensure that a 3.4 mongos can be added to a featureCompatibilityVersion=3.4 cluster.
    assert.commandWorked(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: "3.4"}));
    res = configPrimaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.4");
    assert.eq(
        configPrimaryAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version,
        "3.4");
    st.configRS.awaitReplication();
    res = shardPrimaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.4");
    assert.eq(
        shardPrimaryAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version,
        "3.4");
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
})();
