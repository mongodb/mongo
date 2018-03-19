/**
 * Tests that database versions are properly generated and stored when upgrading from FCV 3.6 to FCV
 * 4.0 in a sharded cluster.
 */

(function() {
    "use strict";

    load("jstests/libs/feature_compatibility_version.js");

    var st = new ShardingTest({
        shards: 1,
        shardOptions: {binVersion: "last-stable"},
    });

    let configPrimaryAdminDB = st.configRS.getPrimary().getDB("admin");
    let shardPrimaryAdminDB = st.rs0.getPrimary().getDB("admin");

    // Make sure config and shards start with last stable FCV
    checkFCV(configPrimaryAdminDB, lastStableFCV);
    checkFCV(shardPrimaryAdminDB, lastStableFCV);

    assert.writeOK(st.s.getDB("test1").foo.insert({_id: "test1", x: 1}));
    assert.writeOK(st.s.getDB("test2").foo.insert({_id: "test2", x: 1}));
    assert.neq(null, st.s.getDB("test1").foo.findOne({_id: "test1", x: 1}));
    assert.neq(null, st.s.getDB("test2").foo.findOne({_id: "test2", x: 1}));

    // Make sure the databases don't have versions when FCV <= 3.6
    let test1 = st.s.getDB("config").getCollection("databases").findOne({_id: "test1"});
    assert(!test1.hasOwnProperty("version"), "db test1 has db version before upgrade");

    let test2 = st.s.getDB("config").getCollection("databases").findOne({_id: "test2"});
    assert(!test2.hasOwnProperty("version"), "db test2 has db version before upgrade");

    // Set FCV to latest
    assert.commandWorked(
        configPrimaryAdminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
    checkFCV(configPrimaryAdminDB, latestFCV);
    checkFCV(shardPrimaryAdminDB, latestFCV);

    // Make sure databases were given versions after upgrade
    test1 = st.s.getDB("config").getCollection("databases").findOne({_id: "test1"});
    assert(test1.hasOwnProperty("version"), "db test1 does not have db version after upgrade");

    test2 = st.s.getDB("config").getCollection("databases").findOne({_id: "test2"});
    assert(test2.hasOwnProperty("version"), "db test2 does not have db version after upgrade");

    st.stop();

})();