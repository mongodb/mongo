//
// Tests sharding-related batch write protocol functionality
// NOTE: Basic write functionality is tested via the passthrough tests, this file should contain
// *only* mongos-specific tests.
//
(function() {
    "use strict";

    var st = new ShardingTest({shards: 2, mongos: 1});

    var mongos = st.s0;
    var admin = mongos.getDB("admin");
    var config = mongos.getDB("config");
    var shards = config.shards.find().toArray();
    var configConnStr = st._configDB;

    jsTest.log("Starting sharding batch write tests...");

    var request;
    var result;

    // NOTE: ALL TESTS BELOW SHOULD BE SELF-CONTAINED, FOR EASIER DEBUGGING

    //
    //
    // Mongos _id autogeneration tests for sharded collections

    var coll = mongos.getCollection("foo.bar");
    assert.commandWorked(admin.runCommand({enableSharding: coll.getDB().toString()}));
    st.ensurePrimaryShard(coll.getDB().getName(), 'shard0001');
    assert.commandWorked(admin.runCommand({shardCollection: coll.toString(), key: {_id: 1}}));

    //
    // Basic insert no _id
    coll.remove({});
    printjson(request = {insert: coll.getName(), documents: [{a: 1}]});
    printjson(result = coll.runCommand(request));
    assert(result.ok);
    assert.eq(1, result.n);
    assert.eq(1, coll.count());

    //
    // Multi insert some _ids
    coll.remove({});
    printjson(request = {insert: coll.getName(), documents: [{_id: 0, a: 1}, {a: 2}]});
    printjson(result = coll.runCommand(request));
    assert(result.ok);
    assert.eq(2, result.n);
    assert.eq(2, coll.count());
    assert.eq(1, coll.count({_id: 0}));

    //
    // Ensure generating many _ids don't push us over limits
    var maxDocSize = (16 * 1024 * 1024) / 1000;
    var baseDocSize = Object.bsonsize({a: 1, data: ""});
    var dataSize = maxDocSize - baseDocSize;

    var data = "";
    for (var i = 0; i < dataSize; i++)
        data += "x";

    var documents = [];
    for (var i = 0; i < 1000; i++)
        documents.push({a: i, data: data});

    assert.commandWorked(coll.getMongo().getDB("admin").runCommand({setParameter: 1, logLevel: 4}));
    coll.remove({});
    request = {insert: coll.getName(), documents: documents};
    printjson(result = coll.runCommand(request));
    assert(result.ok);
    assert.eq(1000, result.n);
    assert.eq(1000, coll.count());

    //
    //
    // Config server upserts (against admin db, for example) require _id test
    var adminColl = admin.getCollection(coll.getName());

    //
    // Without _id
    adminColl.remove({});
    printjson(
        request = {update: adminColl.getName(), updates: [{q: {a: 1}, u: {a: 1}, upsert: true}]});
    var result = adminColl.runCommand(request);
    assert.commandWorked(result);
    assert.eq(1, result.n);
    assert.eq(1, adminColl.count());

    //
    // With _id
    adminColl.remove({});
    printjson(request = {
        update: adminColl.getName(),
        updates: [{q: {_id: 1, a: 1}, u: {a: 1}, upsert: true}]
    });
    assert.commandWorked(adminColl.runCommand(request));
    assert.eq(1, result.n);
    assert.eq(1, adminColl.count());

    //
    //
    // Stale config progress tests
    // Set up a new collection across two shards, then revert the chunks to an earlier state to put
    // mongos and mongod permanently out of sync.

    // START SETUP
    var brokenColl = mongos.getCollection("broken.coll");
    assert.commandWorked(admin.runCommand({enableSharding: brokenColl.getDB().toString()}));
    st.ensurePrimaryShard(brokenColl.getDB().toString(), shards[0]._id);
    assert.commandWorked(admin.runCommand({shardCollection: brokenColl.toString(), key: {_id: 1}}));
    assert.commandWorked(admin.runCommand({split: brokenColl.toString(), middle: {_id: 0}}));

    var oldChunks = config.chunks.find().toArray();

    // Start a new mongos and bring it up-to-date with the chunks so far

    var staleMongos = MongoRunner.runMongos({configdb: configConnStr});
    brokenColl = staleMongos.getCollection(brokenColl.toString());
    assert.writeOK(brokenColl.insert({hello: "world"}));

    // Modify the chunks to make shards at a higher version

    assert.commandWorked(
        admin.runCommand({moveChunk: brokenColl.toString(), find: {_id: 0}, to: shards[1]._id}));

    // Rewrite the old chunks back to the config server

    assert.writeOK(config.chunks.remove({}));
    for (var i = 0; i < oldChunks.length; i++) {
        assert.writeOK(config.chunks.insert(oldChunks[i]));
    }

    // Ensure that the inserts have propagated to all secondary nodes
    st.configRS.awaitReplication();

    // Stale mongos can no longer bring itself up-to-date!
    // END SETUP

    //
    // Config server insert, repeatedly stale
    printjson(request = {insert: brokenColl.getName(), documents: [{_id: -1}]});
    printjson(result = brokenColl.runCommand(request));
    assert(result.ok);
    assert.eq(0, result.n);
    assert.eq(1, result.writeErrors.length);
    assert.eq(0, result.writeErrors[0].index);
    assert.eq(result.writeErrors[0].code, 82);  // No Progress Made

    //
    // Config server insert to other shard, repeatedly stale
    printjson(request = {insert: brokenColl.getName(), documents: [{_id: 1}]});
    printjson(result = brokenColl.runCommand(request));
    assert(result.ok);
    assert.eq(0, result.n);
    assert.eq(1, result.writeErrors.length);
    assert.eq(0, result.writeErrors[0].index);
    assert.eq(result.writeErrors[0].code, 82);  // No Progress Made

    //
    //
    // Tests against config server
    var configColl = config.getCollection("batch_write_protocol_sharded");

    //
    // Basic config server insert
    configColl.remove({});
    printjson(request = {insert: configColl.getName(), documents: [{a: 1}]});
    var result = configColl.runCommand(request);
    assert.commandWorked(result);
    assert.eq(1, result.n);

    st.configRS.awaitReplication();
    assert.eq(1, st.config0.getCollection(configColl + "").count());
    assert.eq(1, st.config1.getCollection(configColl + "").count());
    assert.eq(1, st.config2.getCollection(configColl + "").count());

    //
    // Basic config server update
    configColl.remove({});
    configColl.insert({a: 1});
    printjson(request = {update: configColl.getName(), updates: [{q: {a: 1}, u: {$set: {b: 2}}}]});
    printjson(result = configColl.runCommand(request));
    assert(result.ok);
    assert.eq(1, result.n);

    st.configRS.awaitReplication();
    assert.eq(1, st.config0.getCollection(configColl + "").count({b: 2}));
    assert.eq(1, st.config1.getCollection(configColl + "").count({b: 2}));
    assert.eq(1, st.config2.getCollection(configColl + "").count({b: 2}));

    //
    // Basic config server delete
    configColl.remove({});
    configColl.insert({a: 1});
    printjson(request = {'delete': configColl.getName(), deletes: [{q: {a: 1}, limit: 0}]});
    printjson(result = configColl.runCommand(request));
    assert(result.ok);
    assert.eq(1, result.n);

    st.configRS.awaitReplication();
    assert.eq(0, st.config0.getCollection(configColl + "").count());
    assert.eq(0, st.config1.getCollection(configColl + "").count());
    assert.eq(0, st.config2.getCollection(configColl + "").count());

    MongoRunner.stopMongod(st.config1);
    MongoRunner.stopMongod(st.config2);
    st.configRS.awaitNoPrimary();

    // Config server insert with no config PRIMARY
    configColl.remove({});
    printjson(request = {insert: configColl.getName(), documents: [{a: 1}]});
    printjson(result = configColl.runCommand(request));
    assert(!result.ok);
    assert(result.errmsg != null);

    // Config server insert with no config PRIMARY
    configColl.remove({});
    configColl.insert({a: 1});
    printjson(request = {update: configColl.getName(), updates: [{q: {a: 1}, u: {$set: {b: 2}}}]});
    printjson(result = configColl.runCommand(request));
    assert(!result.ok);
    assert(result.errmsg != null);

    // Config server insert with no config PRIMARY
    configColl.remove({});
    configColl.insert({a: 1});
    printjson(request = {delete: configColl.getName(), deletes: [{q: {a: 1}, limit: 0}]});
    printjson(result = configColl.runCommand(request));
    assert(!result.ok);
    assert(result.errmsg != null);

    jsTest.log("DONE!");

    MongoRunner.stopMongos(staleMongos);
    st.stop();

}());