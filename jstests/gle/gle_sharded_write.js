//
// Ensures GLE correctly reports basic write stats and failures
// Note that test should work correctly with and without write commands.
//
(function() {
    'use strict';

    var st = new ShardingTest({shards: 2, mongos: 1});

    var mongos = st.s0;
    var admin = mongos.getDB("admin");
    var config = mongos.getDB("config");
    var coll = mongos.getCollection(jsTestName() + ".coll");
    var shards = config.shards.find().toArray();

    assert.commandWorked(admin.runCommand({enableSharding: coll.getDB().toString()}));
    printjson(admin.runCommand({movePrimary: coll.getDB().toString(), to: shards[0]._id}));
    assert.commandWorked(admin.runCommand({shardCollection: coll.toString(), key: {_id: 1}}));
    assert.commandWorked(admin.runCommand({split: coll.toString(), middle: {_id: 0}}));
    assert.commandWorked(
        admin.runCommand({moveChunk: coll.toString(), find: {_id: 0}, to: shards[1]._id}));

    st.printShardingStatus();

    var gle = null;

    //
    // Successful insert
    coll.remove({});
    coll.insert({_id: -1});
    printjson(gle = coll.getDB().runCommand({getLastError: 1}));
    assert(gle.ok);
    assert('err' in gle);
    assert(!gle.err);
    assert.eq(coll.count(), 1);

    //
    // Successful update
    coll.remove({});
    coll.insert({_id: 1});
    coll.update({_id: 1}, {$set: {foo: "bar"}});
    printjson(gle = coll.getDB().runCommand({getLastError: 1}));
    assert(gle.ok);
    assert('err' in gle);
    assert(!gle.err);
    assert(gle.updatedExisting);
    assert.eq(gle.n, 1);
    assert.eq(coll.count(), 1);

    //
    // Successful multi-update
    coll.remove({});
    coll.insert({_id: 1});
    coll.update({}, {$set: {foo: "bar"}}, false, true);
    printjson(gle = coll.getDB().runCommand({getLastError: 1}));
    assert(gle.ok);
    assert('err' in gle);
    assert(!gle.err);
    assert(gle.updatedExisting);
    assert.eq(gle.n, 1);
    assert.eq(coll.count(), 1);

    //
    // Successful upsert
    coll.remove({});
    coll.update({_id: 1}, {_id: 1}, true);
    printjson(gle = coll.getDB().runCommand({getLastError: 1}));
    assert(gle.ok);
    assert('err' in gle);
    assert(!gle.err);
    assert(!gle.updatedExisting);
    assert.eq(gle.n, 1);
    assert.eq(gle.upserted, 1);
    assert.eq(coll.count(), 1);

    //
    // Successful upserts
    coll.remove({});
    coll.update({_id: -1}, {_id: -1}, true);
    coll.update({_id: 1}, {_id: 1}, true);
    printjson(gle = coll.getDB().runCommand({getLastError: 1}));
    assert(gle.ok);
    assert('err' in gle);
    assert(!gle.err);
    assert(!gle.updatedExisting);
    assert.eq(gle.n, 1);
    assert.eq(gle.upserted, 1);
    assert.eq(coll.count(), 2);

    //
    // Successful remove
    coll.remove({});
    coll.insert({_id: 1});
    coll.remove({_id: 1});
    printjson(gle = coll.getDB().runCommand({getLastError: 1}));
    assert(gle.ok);
    assert('err' in gle);
    assert(!gle.err);
    assert.eq(gle.n, 1);
    assert.eq(coll.count(), 0);

    //
    // Error on one host during update
    coll.remove({});
    coll.update({_id: 1}, {$invalid: "xxx"}, true);
    printjson(gle = coll.getDB().runCommand({getLastError: 1}));
    assert(gle.ok);
    assert(gle.err);
    assert(gle.code);
    assert(!gle.errmsg);
    assert(gle.singleShard);
    assert.eq(coll.count(), 0);

    //
    // Error on two hosts during remove
    coll.remove({});
    coll.remove({$invalid: 'remove'});
    printjson(gle = coll.getDB().runCommand({getLastError: 1}));
    assert(gle.ok);
    assert(gle.err);
    assert(gle.code);
    assert(!gle.errmsg);
    assert(gle.shards);
    assert.eq(coll.count(), 0);

    //
    // Repeated calls to GLE should work
    coll.remove({});
    coll.update({_id: 1}, {$invalid: "xxx"}, true);
    printjson(gle = coll.getDB().runCommand({getLastError: 1}));
    assert(gle.ok);
    assert(gle.err);
    assert(gle.code);
    assert(!gle.errmsg);
    assert(gle.singleShard);
    printjson(gle = coll.getDB().runCommand({getLastError: 1}));
    assert(gle.ok);
    assert(gle.err);
    assert(gle.code);
    assert(!gle.errmsg);
    assert(gle.singleShard);
    assert.eq(coll.count(), 0);

    //
    // Geo $near is not supported on mongos
    coll.ensureIndex({loc: "2dsphere"});
    coll.remove({});
    var query = {
        loc: {
            $near: {
                $geometry: {type: "Point", coordinates: [0, 0]},
                $maxDistance: 1000,
            }
        }
    };
    printjson(coll.remove(query));
    printjson(gle = coll.getDB().runCommand({getLastError: 1}));
    assert(gle.ok);
    assert(gle.err);
    assert(gle.code);
    assert(!gle.errmsg);
    assert(gle.shards);
    assert.eq(coll.count(), 0);

    //
    // First shard down
    //

    //
    // Successful bulk insert on two hosts, host dies before gle (error contacting host)
    coll.remove({});
    coll.insert([{_id: 1}, {_id: -1}]);
    // Wait for write to be written to shards before shutting it down.
    printjson(gle = coll.getDB().runCommand({getLastError: 1}));
    MongoRunner.stopMongod(st.shard0);
    printjson(gle = coll.getDB().runCommand({getLastError: 1}));
    // Should get an error about contacting dead host.
    assert(!gle.ok);
    assert(gle.errmsg);

    //
    // Failed insert on two hosts, first host dead
    // NOTE: This is DIFFERENT from 2.4, since we don't need to contact a host we didn't get
    // successful writes from.
    coll.remove({_id: 1});
    coll.insert([{_id: 1}, {_id: -1}]);
    printjson(gle = coll.getDB().runCommand({getLastError: 1}));
    assert(gle.ok);
    assert(gle.err);
    assert.eq(coll.count({_id: 1}), 1);

    st.stop();
})();
