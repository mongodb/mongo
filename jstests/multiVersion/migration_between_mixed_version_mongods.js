//
// Testing migrations between latest and last-stable mongod versions, where the
// donor is the latest version and the recipient the last-stable, and vice versa.
// Migrations should be successful.
//

load("./jstests/multiVersion/libs/verify_versions.js");

(function() {
    "use strict";

    var options = {
        shards: [
            {binVersion: "last-stable"},
            {binVersion: "last-stable"},
            {binVersion: "latest"},
            {binVersion: "latest"}
        ],
        mongos: 1,
        other: {mongosOptions: {binVersion: "last-stable"}},
        // TODO: SERVER-24163 remove after v3.4
        waitForCSRSSecondaries: false
    };

    var st = new ShardingTest(options);
    st.stopBalancer();

    assert.binVersion(st.shard0, "last-stable");
    assert.binVersion(st.shard1, "last-stable");
    assert.binVersion(st.shard2, "latest");
    assert.binVersion(st.shard3, "latest");
    assert.binVersion(st.s0, "last-stable");

    var mongos = st.s0, admin = mongos.getDB('admin'),
        shards = mongos.getCollection('config.shards').find().toArray(),

        fooDB = "fooTest", fooNS = fooDB + ".foo", fooColl = mongos.getCollection(fooNS),
        fooDonor = st.shard0, fooRecipient = st.shard2,
        fooDonorColl = fooDonor.getCollection(fooNS),
        fooRecipientColl = fooRecipient.getCollection(fooNS),

        barDB = "barTest", barNS = barDB + ".foo", barColl = mongos.getCollection(barNS),
        barDonor = st.shard3, barRecipient = st.shard1,
        barDonorColl = barDonor.getCollection(barNS),
        barRecipientColl = barRecipient.getCollection(barNS);

    assert.commandWorked(admin.runCommand({enableSharding: fooDB}));
    assert.commandWorked(admin.runCommand({enableSharding: barDB}));
    st.ensurePrimaryShard(fooDB, shards[0]._id);
    st.ensurePrimaryShard(barDB, shards[3]._id);

    assert.commandWorked(admin.runCommand({shardCollection: fooNS, key: {a: 1}}));
    assert.commandWorked(admin.runCommand({split: fooNS, middle: {a: 10}}));
    assert.commandWorked(admin.runCommand({shardCollection: barNS, key: {a: 1}}));
    assert.commandWorked(admin.runCommand({split: barNS, middle: {a: 10}}));

    fooColl.insert({a: 0});
    assert.eq(null, fooColl.getDB().getLastError());
    fooColl.insert({a: 10});
    assert.eq(null, fooColl.getDB().getLastError());
    assert.eq(0, fooRecipientColl.count());
    assert.eq(2, fooDonorColl.count());
    assert.eq(2, fooColl.count());

    barColl.insert({a: 0});
    assert.eq(null, barColl.getDB().getLastError());
    barColl.insert({a: 10});
    assert.eq(null, barColl.getDB().getLastError());
    assert.eq(0, barRecipientColl.count());
    assert.eq(2, barDonorColl.count());
    assert.eq(2, barColl.count());

    /**
     * Perform two migrations:
     *      shard0 (last-stable) -> foo chunk -> shard2 (latest)
     *      shard3 (latest)      -> bar chunk -> shard1 (last-stable)
     */

    assert.commandWorked(admin.runCommand({moveChunk: fooNS, find: {a: 10}, to: shards[2]._id}));
    assert.commandWorked(admin.runCommand({moveChunk: barNS, find: {a: 10}, to: shards[1]._id}));
    assert.eq(1,
              fooRecipientColl.count(),
              "Foo collection migration failed. " +
                  "Last-stable -> latest mongod version migration failure.");
    assert.eq(1,
              fooDonorColl.count(),
              "Foo donor lost its document. " +
                  "Last-stable -> latest mongod version migration failure.");
    assert.eq(2,
              fooColl.count(),
              "Incorrect number of documents in foo collection. " +
                  "Last-stable -> latest mongod version migration failure.");
    assert.eq(1,
              barRecipientColl.count(),
              "Bar collection migration failed. " +
                  "Latest -> last-stable mongod version migration failure.");
    assert.eq(1,
              barDonorColl.count(),
              "Bar donor lost its document. " +
                  "Latest -> last-stable mongod version migration failure.");
    assert.eq(2,
              barColl.count(),
              "Incorrect number of documents in bar collection. " +
                  "Latest -> last-stable mongod version migration failure.");

    st.stop();
})();
