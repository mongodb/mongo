// Tests for setFeatureCompatibilityVersion.
(function() {
    "use strict";

    load("jstests/replsets/rslib.js");
    load("jstests/libs/get_index_helpers.js");
    load("jstests/libs/override_methods/multiversion_override_balancer_control.js");

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

    // There should be a v=2 index on the "admin.system.version" collection when the
    // featureCompatibilityVersion is initialized to 3.4. Additionally, the index version of this
    // index should be returned from the "listIndexes" command as a decimal value.
    var allIndexes = adminDB.system.version.getIndexes();
    var spec = GetIndexHelpers.findByName(allIndexes, "incompatible_with_version_32");
    assert.neq(null,
               spec,
               "Index with name 'incompatible_with_version_32' not found: " + tojson(allIndexes));
    assert.eq(new NumberDecimal("2"), spec.v, tojson(spec));

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

    // setFeatureCompatibilityVersion fails to downgrade to FCV=3.2 if the write fails.
    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: "failCollectionUpdates",
        data: {collectionNS: "admin.system.version"},
        mode: "alwaysOn"
    }));
    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: "3.2"}));
    res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.4");
    assert.eq(adminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version, "3.4");
    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: "failCollectionUpdates",
        data: {collectionNS: "admin.system.version"},
        mode: "off"
    }));

    // featureCompatibilityVersion can be set to 3.2.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.2"}));
    res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.2");
    assert.eq(adminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version, "3.2");

    // The v=2 index that was created on the "admin.system.version" collection should be removed
    // when the featureCompatibilityVersion is set to 3.2.
    allIndexes = adminDB.system.version.getIndexes();
    spec = GetIndexHelpers.findByName(allIndexes, "incompatible_with_version_32");
    assert.eq(null,
              spec,
              "Expected index with name 'incompatible_with_version_32' to have been removed: " +
                  tojson(allIndexes));

    // setFeatureCompatibilityVersion fails to upgrade to FCV=3.4 if the write fails.
    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: "failCollectionUpdates",
        data: {collectionNS: "admin.system.version"},
        mode: "alwaysOn"
    }));
    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: "3.4"}));
    res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.2");
    assert.eq(adminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version, "3.2");
    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: "failCollectionUpdates",
        data: {collectionNS: "admin.system.version"},
        mode: "off"
    }));

    // featureCompatibilityVersion can be set to 3.4.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.4"}));
    res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.4");
    assert.eq(adminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version, "3.4");

    // There should be a v=2 index on the "admin.system.version" collection when the
    // featureCompatibilityVersion is set to 3.4. Additionally, the index version of this index
    // should be returned from the "listIndexes" command as a decimal value.
    allIndexes = adminDB.system.version.getIndexes();
    spec = GetIndexHelpers.findByName(allIndexes, "incompatible_with_version_32");
    assert.neq(null,
               spec,
               "Index with name 'incompatible_with_version_32' not found: " + tojson(allIndexes));
    assert.eq(new NumberDecimal("2"), spec.v, tojson(spec));

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

    // Test that a 3.2 secondary crashes when performing an initial sync from a 3.4 primary with
    // featureCompatibilityVersion=3.4.
    rst = new ReplSetTest({nodes: [{binVersion: latest}, {binVersion: downgrade}]});
    rst.startSet();

    // Rig the election so that the node running latest version becomes the primary. Give the 3.2
    // node no votes, so that the primary doesn't step down when it crashes.
    replSetConfig = rst.getReplSetConfig();
    replSetConfig.members[1].priority = 0;
    replSetConfig.members[1].votes = 0;

    // TODO(SERVER-14017): remove this in favor of using initiate() everywhere.
    rst.initiateWithAnyNodeAsPrimary(replSetConfig);
    rst.awaitSecondaryNodes();

    primaryAdminDB = rst.getPrimary().getDB("admin");
    res = assert.commandWorked(
        primaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1}));
    assert.eq(res.featureCompatibilityVersion, "3.4", tojson(res));

    // Verify that the 3.2 secondary terminates when the "listIndexes" command returns decimal data.
    secondaryAdminDB = rst.getSecondary().getDB("admin");
    assert.soon(
        function() {
            try {
                secondaryAdminDB.runCommand({ping: 1});
            } catch (e) {
                return true;
            }
            return false;
        },
        function() {
            return "Expected 3.2 secondary to terminate due to reading decimal data, but it" +
                " didn't. Indexes on the 3.2 secondary's admin.system.version collection: " +
                tojson(secondaryAdminDB.system.version.getIndexes());
        });
    rst.stopSet(undefined, undefined, {allowedExitCodes: [MongoRunner.EXIT_ABRUPT]});

    // Test that a 3.2 secondary can successfully perform initial sync from a 3.4 primary with
    // featureCompatibilityVersion=3.2.
    rst = new ReplSetTest({nodes: [{binVersion: latest}]});
    rst.startSet();
    rst.initiate();

    primaryAdminDB = rst.getPrimary().getDB("admin");
    assert.commandWorked(primaryAdminDB.runCommand({setFeatureCompatibilityVersion: "3.2"}));
    secondaryAdminDB = rst.add({binVersion: downgrade}).getDB("admin");

    // We also add a 3.4 secondary so that a majority of the replica set is still available after
    // setting the featureCompatibilityVersion to 3.4.
    rst.add({binVersion: latest});

    // Rig the election so that the first node running latest version remains the primary after the
    // 3.2 secondary is added to the replica set.
    replSetConfig = rst.getReplSetConfig();
    replSetConfig.version = 2;
    replSetConfig.members[1].priority = 0;
    replSetConfig.members[2].priority = 0;
    reconfig(rst, replSetConfig);

    // Verify that the 3.2 secondary successfully performed its initial sync.
    assert.writeOK(
        primaryAdminDB.getSiblingDB("test").coll.insert({awaitRepl: true}, {writeConcern: {w: 3}}));

    // Test that a 3.2 secondary crashes when syncing from a 3.4 primary and the
    // featureCompatibilityVersion is set to 3.4.
    assert.commandWorked(primaryAdminDB.runCommand({setFeatureCompatibilityVersion: "3.4"}));
    assert.soon(
        function() {
            try {
                secondaryAdminDB.runCommand({ping: 1});
            } catch (e) {
                return true;
            }
            return false;
        },
        function() {
            return "Expected 3.2 secondary to terminate due to attempting to build a v=2 index," +
                " but it didn't. Indexes on the 3.2 secondary's admin.system.version" +
                " collection: " + tojson(secondaryAdminDB.system.version.getIndexes());
        });
    rst.stopSet(undefined, undefined, {allowedExitCodes: [MongoRunner.EXIT_ABRUPT]});

    //
    // Sharding tests.
    //

    var st;
    var mongosAdminDB;
    var configPrimaryAdminDB;
    var shardPrimaryAdminDB;

    // New 3.4 cluster.
    st = new ShardingTest({
        shards: {rs0: {nodes: [{binVersion: latest}, {binVersion: latest}]}},
        other: {useBridge: true}
    });
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

    // Prevent the shard primary from receiving messages from the config server primary. When we try
    // to set the featureCompatibilityVersion to "3.2", it should fail because the shard cannot be
    // contacted. The config server primary should still report "3.4", since setting the version to
    // "3.2" did not succeed.
    st.rs0.getPrimary().discardMessagesFrom(st.configRS.getPrimary(), 1.0);
    assert.commandFailed(
        mongosAdminDB.runCommand({setFeatureCompatibilityVersion: "3.2", maxTimeMS: 1000}));
    res = configPrimaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.4");
    st.rs0.getPrimary().discardMessagesFrom(st.configRS.getPrimary(), 0.0);

    // featureCompatibilityVersion can be set to 3.2 on mongos.
    // This is run through assert.soon() because we've just caused a network interruption
    // by discarding messages in the bridge.
    assert.soon(function() {
        var res = mongosAdminDB.runCommand({setFeatureCompatibilityVersion: "3.2"});
        if (res.ok == 0) {
            print("Failed to set feature compatibility version: " + tojson(res));
            return false;
        }
        return true;
    });

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

    // Create a cluster running with featureCompatibilityVersion=3.4.
    st = new ShardingTest({shards: 1, mongos: 1});
    mongosAdminDB = st.s.getDB("admin");
    configPrimaryAdminDB = st.configRS.getPrimary().getDB("admin");
    shardPrimaryAdminDB = st.shard0.getDB("admin");
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

    // Ensure that a 3.2 mongos fails to join the featureCompatibilityVersion=3.4 cluster.
    var downgradeMongos =
        MongoRunner.runMongos({configdb: st.configRS.getURL(), binVersion: downgrade});
    assert.eq(null, downgradeMongos);

    // Ensure that a 3.2 mongos can be added to a featureCompatibilityVersion=3.2 cluster.
    assert.commandWorked(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: "3.2"}));
    res = configPrimaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.2");
    assert.eq(
        configPrimaryAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version,
        "3.2");
    st.configRS.awaitReplication();
    res = shardPrimaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.2");
    assert.eq(
        shardPrimaryAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version,
        "3.2");
    downgradeMongos =
        MongoRunner.runMongos({configdb: st.configRS.getURL(), binVersion: downgrade});
    assert.neq(null, downgradeMongos);

    // Ensure that the 3.2 mongos can perform reads and writes to the shards in the cluster.
    assert.writeOK(downgradeMongos.getDB("test").foo.insert({x: 1}));
    var foundDoc = downgradeMongos.getDB("test").foo.findOne({x: 1});
    assert.neq(null, foundDoc);
    assert.eq(1, foundDoc.x, tojson(foundDoc));
    st.stop();
})();
