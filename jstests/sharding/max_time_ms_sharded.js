// Test mongos implementation of time-limited operations: verify that mongos correctly forwards max
// time to shards, and that mongos correctly times out max time sharded getmore operations (which
// are run in series on shards).
//
// Note that mongos does not time out commands or query ops (which remains responsibility of mongod,
// pending development of an interrupt framework for mongos).
(function() {
    'use strict';

    var st = new ShardingTest({shards: 2});

    var mongos = st.s0;
    var shards = [st.shard0, st.shard1];
    var coll = mongos.getCollection("foo.bar");
    var admin = mongos.getDB("admin");
    var exceededTimeLimit = 50;  // ErrorCodes::ExceededTimeLimit
    var cursor;
    var res;

    // Helper function to configure "maxTimeAlwaysTimeOut" fail point on shards, which forces mongod
    // to
    // throw if it receives an operation with a max time.  See fail point declaration for complete
    // description.
    var configureMaxTimeAlwaysTimeOut = function(mode) {
        assert.commandWorked(shards[0].getDB("admin").runCommand(
            {configureFailPoint: "maxTimeAlwaysTimeOut", mode: mode}));
        assert.commandWorked(shards[1].getDB("admin").runCommand(
            {configureFailPoint: "maxTimeAlwaysTimeOut", mode: mode}));
    };

    // Helper function to configure "maxTimeAlwaysTimeOut" fail point on shards, which prohibits
    // mongod
    // from enforcing time limits.  See fail point declaration for complete description.
    var configureMaxTimeNeverTimeOut = function(mode) {
        assert.commandWorked(shards[0].getDB("admin").runCommand(
            {configureFailPoint: "maxTimeNeverTimeOut", mode: mode}));
        assert.commandWorked(shards[1].getDB("admin").runCommand(
            {configureFailPoint: "maxTimeNeverTimeOut", mode: mode}));
    };

    //
    // Pre-split collection: shard 0 takes {_id: {$lt: 0}}, shard 1 takes {_id: {$gte: 0}}.
    //
    assert.commandWorked(admin.runCommand({enableSharding: coll.getDB().getName()}));
    st.ensurePrimaryShard(coll.getDB().toString(), "shard0000");
    assert.commandWorked(admin.runCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
    assert.commandWorked(admin.runCommand({split: coll.getFullName(), middle: {_id: 0}}));
    assert.commandWorked(
        admin.runCommand({moveChunk: coll.getFullName(), find: {_id: 0}, to: "shard0001"}));

    //
    // Insert 100 documents into sharded collection, such that each shard owns 50.
    //
    var bulk = coll.initializeUnorderedBulkOp();
    for (var i = -50; i < 50; i++) {
        bulk.insert({_id: i});
    }
    assert.writeOK(bulk.execute());
    assert.eq(50, shards[0].getCollection(coll.getFullName()).count());
    assert.eq(50, shards[1].getCollection(coll.getFullName()).count());

    //
    // Test that mongos correctly forwards max time to shards for sharded queries.  Uses
    // maxTimeAlwaysTimeOut to ensure mongod throws if it receives a max time.
    //

    // Positive test.
    configureMaxTimeAlwaysTimeOut("alwaysOn");
    cursor = coll.find();
    cursor.maxTimeMS(60 * 1000);
    assert.throws(function() {
        cursor.next();
    }, [], "expected query to fail in mongod due to maxTimeAlwaysTimeOut fail point");

    // Negative test.
    configureMaxTimeAlwaysTimeOut("off");
    cursor = coll.find();
    cursor.maxTimeMS(60 * 1000);
    assert.doesNotThrow(function() {
        cursor.next();
    }, [], "expected query to not hit time limit in mongod");

    //
    // Test that mongos correctly times out max time sharded getmore operations.  Uses
    // maxTimeNeverTimeOut to ensure mongod doesn't enforce a time limit.
    //
    // TODO: This is unimplemented.  A test for this functionality should be written as
    // part of the work for SERVER-19410.
    //

    configureMaxTimeNeverTimeOut("alwaysOn");

    // Positive test.  TODO: see above.

    // Negative test.  ~10s operation, with a high (1-day) limit.
    cursor = coll.find({
        $where: function() {
            sleep(100);
            return true;
        }
    });
    cursor.batchSize(2);
    cursor.maxTimeMS(1000 * 60 * 60 * 24);
    assert.doesNotThrow(function() {
        cursor.next();
    }, [], "did not expect mongos to time out first batch of query");
    assert.doesNotThrow(function() {
        cursor.itcount();
    }, [], "did not expect getmore ops to hit the time limit");

    configureMaxTimeNeverTimeOut("off");

    //
    // Test that mongos correctly forwards max time to shards for sharded commands.  Uses
    // maxTimeAlwaysTimeOut to ensure mongod throws if it receives a max time.
    //

    // Positive test for "validate".
    configureMaxTimeAlwaysTimeOut("alwaysOn");
    res = coll.runCommand("validate", {maxTimeMS: 60 * 1000});
    assert.commandFailed(
        res, "expected validate to fail in mongod due to maxTimeAlwaysTimeOut fail point");
    assert.eq(res["code"],
              exceededTimeLimit,
              "expected code " + exceededTimeLimit + " from validate, instead got: " + tojson(res));

    // Negative test for "validate".
    configureMaxTimeAlwaysTimeOut("off");
    assert.commandWorked(coll.runCommand("validate", {maxTimeMS: 60 * 1000}),
                         "expected validate to not hit time limit in mongod");

    // Positive test for "count".
    configureMaxTimeAlwaysTimeOut("alwaysOn");
    res = coll.runCommand("count", {maxTimeMS: 60 * 1000});
    assert.commandFailed(res,
                         "expected count to fail in mongod due to maxTimeAlwaysTimeOut fail point");
    assert.eq(res["code"],
              exceededTimeLimit,
              "expected code " + exceededTimeLimit + " from count , instead got: " + tojson(res));

    // Negative test for "count".
    configureMaxTimeAlwaysTimeOut("off");
    assert.commandWorked(coll.runCommand("count", {maxTimeMS: 60 * 1000}),
                         "expected count to not hit time limit in mongod");

    // Positive test for "collStats".
    configureMaxTimeAlwaysTimeOut("alwaysOn");
    res = coll.runCommand("collStats", {maxTimeMS: 60 * 1000});
    assert.commandFailed(
        res, "expected collStats to fail in mongod due to maxTimeAlwaysTimeOut fail point");
    assert.eq(
        res["code"],
        exceededTimeLimit,
        "expected code " + exceededTimeLimit + " from collStats, instead got: " + tojson(res));

    // Negative test for "collStats".
    configureMaxTimeAlwaysTimeOut("off");
    assert.commandWorked(coll.runCommand("collStats", {maxTimeMS: 60 * 1000}),
                         "expected collStats to not hit time limit in mongod");

    // Positive test for "mapReduce".
    configureMaxTimeAlwaysTimeOut("alwaysOn");
    res = coll.runCommand("mapReduce", {
        map: function() {
            emit(0, 0);
        },
        reduce: function(key, values) {
            return 0;
        },
        out: {inline: 1},
        maxTimeMS: 60 * 1000
    });
    assert.commandFailed(
        res, "expected mapReduce to fail in mongod due to maxTimeAlwaysTimeOut fail point");
    assert.eq(
        res["code"],
        exceededTimeLimit,
        "expected code " + exceededTimeLimit + " from mapReduce, instead got: " + tojson(res));

    // Negative test for "mapReduce".
    configureMaxTimeAlwaysTimeOut("off");
    assert.commandWorked(coll.runCommand("mapReduce", {
        map: function() {
            emit(0, 0);
        },
        reduce: function(key, values) {
            return 0;
        },
        out: {inline: 1},
        maxTimeMS: 60 * 1000
    }),
                         "expected mapReduce to not hit time limit in mongod");

    // Positive test for "aggregate".
    configureMaxTimeAlwaysTimeOut("alwaysOn");
    res = coll.runCommand("aggregate", {pipeline: [], maxTimeMS: 60 * 1000});
    assert.commandFailed(
        res, "expected aggregate to fail in mongod due to maxTimeAlwaysTimeOut fail point");
    assert.eq(
        res["code"],
        exceededTimeLimit,
        "expected code " + exceededTimeLimit + " from aggregate , instead got: " + tojson(res));

    // Negative test for "aggregate".
    configureMaxTimeAlwaysTimeOut("off");
    assert.commandWorked(coll.runCommand("aggregate", {pipeline: [], maxTimeMS: 60 * 1000}),
                         "expected aggregate to not hit time limit in mongod");

    // Positive test for "moveChunk".
    configureMaxTimeAlwaysTimeOut("alwaysOn");
    res = admin.runCommand({
        moveChunk: coll.getFullName(),
        find: {_id: 0},
        to: "shard0000",
        maxTimeMS: 1000 * 60 * 60 * 24
    });
    assert.commandFailed(
        res, "expected moveChunk to fail in mongod due to maxTimeAlwaysTimeOut fail point");
    assert.eq(
        res["code"],
        exceededTimeLimit,
        "expected code " + exceededTimeLimit + " from moveChunk, instead got: " + tojson(res));

    // Negative test for "moveChunk".
    configureMaxTimeAlwaysTimeOut("off");
    assert.commandWorked(admin.runCommand({
        moveChunk: coll.getFullName(),
        find: {_id: 0},
        to: "shard0000",
        maxTimeMS: 1000 * 60 * 60 * 24
    }),
                         "expected moveChunk to not hit time limit in mongod");

    // TODO Test additional commmands.

    st.stop();

})();
