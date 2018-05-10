/**
 * Tests that chunk histories are properly generated and stored when upgrading from FCV 3.6 to FCV
 * 4.0 in a sharded cluster.
 */

(function() {
    "use strict";

    load("jstests/libs/feature_compatibility_version.js");

    var st = new ShardingTest({
        shards: 1,
    });

    let configPrimaryAdminDB = st.configRS.getPrimary().getDB("admin");
    let shardPrimaryAdminDB = st.rs0.getPrimary().getDB("admin");

    // Change FCV to last stable so chunks will not have histories.
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: "3.6"}));
    checkFCV(configPrimaryAdminDB, "3.6");
    checkFCV(shardPrimaryAdminDB, "3.6");

    let testDB = st.s.getDB("test1");

    // Create a sharded collection with primary shard 0.
    assert.commandWorked(st.s.adminCommand({enableSharding: testDB.getName()}));
    st.ensurePrimaryShard(testDB.getName(), st.shard0.shardName);
    assert.commandWorked(
        st.s.adminCommand({shardCollection: testDB.foo.getFullName(), key: {a: 1}}));
    assert.commandWorked(st.s.adminCommand({split: testDB.foo.getFullName(), middle: {a: 0}}));

    assert.writeOK(st.s.getDB("test1").foo.insert({_id: "id1", a: 1}));
    assert.neq(null, st.s.getDB("test1").foo.findOne({_id: "id1", a: 1}));

    assert.writeOK(st.s.getDB("test1").foo.insert({_id: "id2", a: -1}));
    assert.neq(null, st.s.getDB("test1").foo.findOne({_id: "id2", a: -1}));

    // Make sure chunks do not have history when FCV <= 3.6.
    let chunks = st.s.getDB("config").getCollection("chunks").find({ns: "test1.foo"}).toArray();
    assert.eq(chunks.length, 2);
    chunks.forEach((chunk) => {
        assert.neq(null, chunk);
        assert(!chunk.hasOwnProperty("history"), "test1.foo has a history before upgrade");
    });

    // Set FCV to latest.
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: "4.0"}));
    checkFCV(configPrimaryAdminDB, latestFCV);
    checkFCV(shardPrimaryAdminDB, latestFCV);

    // Make sure chunks for test1.foo were given history after upgrade.
    chunks = st.s.getDB("config").getCollection("chunks").find({ns: "test1.foo"}).toArray();
    assert.eq(chunks.length, 2);
    chunks.forEach((chunk) => {
        assert.neq(null, chunk);
        assert(chunk.hasOwnProperty("history"), "test1.foo does not have a history after upgrade");
    });

    // Set FCV to last-stable.
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: "3.6"}));
    checkFCV(configPrimaryAdminDB, "3.6");
    checkFCV(shardPrimaryAdminDB, "3.6");

    // Make sure history was removed when FCV changed to <= 3.6.
    chunks = st.s.getDB("config").getCollection("chunks").find({ns: "test1.foo"}).toArray();
    assert.eq(chunks.length, 2);
    chunks.forEach((chunk) => {
        assert.neq(null, chunk);
        assert(!chunk.hasOwnProperty("history"), "test1.foo has a history after downgrade");
    });

    st.stop();

})();