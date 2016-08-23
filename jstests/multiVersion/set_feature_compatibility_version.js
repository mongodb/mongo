// Tests for setFeatureCompatibilityVersion.
(function() {
    "use strict";

    load('jstests/libs/override_methods/multiversion_override_balancer_control.js');

    var res;
    const latest = "latest";
    const downgrade = "3.2";

    //
    // Standalone tests.
    //

    var dbpath = MongoRunner.dataPath + "feature_compatibility_version";
    resetDbpath(dbpath);
    var conn;
    var adminDB;

    // New 3.4 standalone.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest});
    assert.neq(null, conn);
    adminDB = conn.getDB("admin");

    // Initially featureCompatibilityVersion is 3.4.
    res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.4");
    assert.eq(adminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version, "3.4");

    // featureCompatibilityVersion cannot be set to invalid value.
    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: 5}));
    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: "3.0"}));
    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: "3.6"}));

    // setFeatureCompatibilityVersion rejects unknown fields.
    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: "3.2", unknown: 1}));

    // setFeatureCompatibilityVersion can only be run on the admin database.
    assert.commandFailed(conn.getDB("test").runCommand({setFeatureCompatibilityVersion: "3.2"}));

    // featureCompatibilityVersion cannot be set via setParameter.
    assert.commandFailed(adminDB.runCommand({setParameter: 1, featureCompatibilityVersion: "3.2"}));

    // featureCompatibilityVersion can be set to 3.2.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.2"}));
    res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.2");
    assert.eq(adminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version, "3.2");

    // featureCompatibilityVersion can be set to 3.4.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.4"}));
    res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.4");
    assert.eq(adminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version, "3.4");

    MongoRunner.stopMongod(conn);

    // featureCompatibilityVersion is durable.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest});
    assert.neq(null, conn);
    adminDB = conn.getDB("admin");
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.2"}));
    MongoRunner.stopMongod(conn);

    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest, noCleanData: true});
    assert.neq(null, conn);
    adminDB = conn.getDB("admin");
    res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.2");
    assert.eq(adminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version, "3.2");
    MongoRunner.stopMongod(conn);

    // If you upgrade from 3.2 to 3.4 and have non-local databases, featureCompatibilityVersion is
    // 3.2.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: downgrade});
    assert.neq(null, conn);
    assert.writeOK(conn.getDB("test").coll.insert({a: 5}));
    MongoRunner.stopMongod(conn);

    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest, noCleanData: true});
    assert.neq(null, conn);
    adminDB = conn.getDB("admin");
    res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.2");
    MongoRunner.stopMongod(conn);

    //
    // Replica set tests.
    //

    var rst;
    var rstConns;
    var replSetConfig;
    var primaryAdminDB;
    var secondaryAdminDB;

    // New 3.4 replica set.
    rst = new ReplSetTest({nodes: 2, nodeOpts: {binVersion: latest}});
    rst.startSet();
    rst.initiate();
    primaryAdminDB = rst.getPrimary().getDB("admin");
    secondaryAdminDB = rst.getSecondary().getDB("admin");

    // Initially featureCompatibilityVersion is 3.4 on primary and secondary.
    var ex;
    assert.soon(
        function() {
            try {
                res = primaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
                assert.commandWorked(res);
                assert.eq(res.featureCompatibilityVersion, "3.4");
                assert.eq(
                    primaryAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"})
                        .version,
                    "3.4");
                return true;
            } catch (e) {
                ex = e;
                return false;
            }
        },
        function() {
            return ex.toString();
        });
    rst.awaitReplication();
    res = secondaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.4");
    assert.eq(secondaryAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version,
              "3.4");

    // featureCompatibilityVersion propagates to secondary.
    assert.commandWorked(primaryAdminDB.runCommand({setFeatureCompatibilityVersion: "3.2"}));
    res = primaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.2");
    assert.eq(primaryAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version,
              "3.2");
    rst.awaitReplication();
    res = secondaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.2");
    assert.eq(secondaryAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version,
              "3.2");

    // setFeatureCompatibilityVersion cannot be run on secondary.
    assert.commandFailed(secondaryAdminDB.runCommand({setFeatureCompatibilityVersion: "3.4"}));

    rst.stopSet();

    // A 3.4 secondary with a 3.2 primary will have featureCompatibilityVersion=3.2.
    rst = new ReplSetTest({nodes: [{binVersion: downgrade}, {binVersion: latest}]});
    rstConns = rst.startSet();
    replSetConfig = rst.getReplSetConfig();
    replSetConfig.members[0].priority = 2;
    replSetConfig.members[1].priority = 1;
    rst.initiate(replSetConfig);
    rst.waitForState(rstConns[0], ReplSetTest.State.PRIMARY);
    secondaryAdminDB = rst.getSecondary().getDB("admin");
    res = secondaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.2");
    rst.stopSet();

    //
    // Sharding tests.
    //

    var st;
    var mongosAdminDB;
    var configPrimaryAdminDB;
    var shardPrimaryAdminDB;

    // New 3.4 cluster.
    st = new ShardingTest({shards: {rs0: {nodes: [{binVersion: latest}, {binVersion: latest}]}}});
    mongosAdminDB = st.s.getDB("admin");
    configPrimaryAdminDB = st.configRS.getPrimary().getDB("admin");
    shardPrimaryAdminDB = st.rs0.getPrimary().getDB("admin");

    // Initially featureCompatibilityVersion is 3.4 on config and shard.
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

    // featureCompatibilityVersion cannot be set to invalid value on mongos.
    assert.commandFailed(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: 5}));
    assert.commandFailed(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: "3.0"}));
    assert.commandFailed(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: "3.6"}));

    // setFeatureCompatibilityVersion rejects unknown fields on mongos.
    assert.commandFailed(
        mongosAdminDB.runCommand({setFeatureCompatibilityVersion: "3.2", unknown: 1}));

    // setFeatureCompatibilityVersion can only be run on the admin database on mongos.
    assert.commandFailed(st.s.getDB("test").runCommand({setFeatureCompatibilityVersion: "3.2"}));

    // featureCompatibilityVersion cannot be set via setParameter on mongos.
    assert.commandFailed(
        mongosAdminDB.runCommand({setParameter: 1, featureCompatibilityVersion: "3.2"}));

    // featureCompatibilityVersion can be set to 3.2 on mongos.
    assert.commandWorked(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: "3.2"}));

    // featureCompatibilityVersion propagates to config and shard.
    res = configPrimaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.2");
    assert.eq(
        configPrimaryAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version,
        "3.2");
    res = shardPrimaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.2");
    assert.eq(
        shardPrimaryAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version,
        "3.2");

    // A 3.4 shard added to a cluster with featureCompatibilityVersion=3.2 gets
    // featureCompatibilityVersion=3.2.
    var latestShard = new ReplSetTest({
        name: "latestShard",
        nodes: [{binVersion: latest}, {binVersion: latest}],
        nodeOptions: {shardsvr: ""},
        useHostName: true
    });
    latestShard.startSet();
    latestShard.initiate();
    assert.commandWorked(mongosAdminDB.runCommand({addShard: latestShard.getURL()}));
    var latestShardPrimaryAdminDB = latestShard.getPrimary().getDB("admin");
    res = latestShardPrimaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.2");

    // featureCompatibilityVersion can be set to 3.4 on mongos.
    assert.commandWorked(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: "3.4"}));
    res = st.configRS.getPrimary().getDB("admin").runCommand(
        {getParameter: 1, featureCompatibilityVersion: 1});
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

    latestShard.stopSet();
    st.stop();

    // Create cluster with 3.2 mongos so that we can add 3.2 shards.
    st = new ShardingTest({shards: 0, other: {mongosOptions: {binVersion: downgrade}}});
    mongosAdminDB = st.s.getDB("admin");
    configPrimaryAdminDB = st.configRS.getPrimary().getDB("admin");

    // Adding a 3.2 shard to a cluster with featureCompatibilityVersion=3.2 succeeds.
    res = configPrimaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.2");
    assert.eq(
        configPrimaryAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version,
        "3.2");
    var downgradeShard = new ReplSetTest({
        name: "downgradeShard",
        nodes: [{binVersion: downgrade}, {binVersion: downgrade}],
        nodeOptions: {shardsvr: ""},
        useHostName: true
    });
    downgradeShard.startSet();
    downgradeShard.initiate();
    assert.commandWorked(mongosAdminDB.runCommand({addShard: downgradeShard.getURL()}));

    downgradeShard.stopSet();
    st.stop();
})();
