//
// Basic tests of cleanupOrphaned. Validates that non allowed uses of the cleanupOrphaned
// command fail.
//

(function() {
    "use strict";

    /*****************************************************************************
     * Unsharded mongod.
     ****************************************************************************/

    // cleanupOrphaned fails against unsharded mongod.
    var mongod = MongoRunner.runMongod();
    assert.commandFailed(mongod.getDB('admin').runCommand({cleanupOrphaned: 'foo.bar'}));

    /*****************************************************************************
     * Bad invocations of cleanupOrphaned command.
     ****************************************************************************/

    var st = new ShardingTest({other: {rs: true, rsOptions: {nodes: 2}}});

    var mongos = st.s0;
    var mongosAdmin = mongos.getDB('admin');
    var dbName = 'foo';
    var collectionName = 'bar';
    var ns = dbName + '.' + collectionName;
    var coll = mongos.getCollection(ns);

    // cleanupOrphaned fails against mongos ('no such command'): it must be run
    // on mongod.
    assert.commandFailed(mongosAdmin.runCommand({cleanupOrphaned: ns}));

    // cleanupOrphaned must be run on admin DB.
    var shardFooDB = st.shard0.getDB(dbName);
    assert.commandFailed(shardFooDB.runCommand({cleanupOrphaned: ns}));

    // Must be run on primary.
    var secondaryAdmin = st.rs0.getSecondary().getDB('admin');
    var response = secondaryAdmin.runCommand({cleanupOrphaned: ns});
    print('cleanupOrphaned on secondary:');
    printjson(response);
    assert.commandFailed(response);

    var shardAdmin = st.shard0.getDB('admin');
    var badNS = ' \\/."*<>:|?';
    assert.commandFailed(shardAdmin.runCommand({cleanupOrphaned: badNS}));

    // cleanupOrphaned works on sharded collection.
    assert.commandWorked(mongosAdmin.runCommand({enableSharding: coll.getDB().getName()}));

    st.ensurePrimaryShard(coll.getDB().getName(), st.shard0.shardName);

    assert.commandWorked(mongosAdmin.runCommand({shardCollection: ns, key: {_id: 1}}));

    assert.commandWorked(shardAdmin.runCommand({cleanupOrphaned: ns}));

    /*****************************************************************************
     * Empty shard.
     ****************************************************************************/

    // Ping shard[1] so it will be aware that it is sharded. Otherwise cleanupOrphaned
    // may fail.
    assert.commandWorked(mongosAdmin.runCommand({
        moveChunk: coll.getFullName(),
        find: {_id: 1},
        to: st.shard1.shardName,
        _waitForDelete: true
    }));

    assert.commandWorked(mongosAdmin.runCommand({
        moveChunk: coll.getFullName(),
        find: {_id: 1},
        to: st.shard0.shardName,
        _waitForDelete: true
    }));

    // Collection's home is shard0, there are no chunks assigned to shard1.
    st.shard1.getCollection(ns).insert({});
    assert.eq(null, st.shard1.getDB(dbName).getLastError());
    assert.eq(1, st.shard1.getCollection(ns).count());
    response = st.shard1.getDB('admin').runCommand({cleanupOrphaned: ns});
    assert.commandWorked(response);
    assert.eq({_id: {$maxKey: 1}}, response.stoppedAtKey);
    assert.eq(0,
              st.shard1.getCollection(ns).count(),
              "cleanupOrphaned didn't delete orphan on empty shard.");

    /*****************************************************************************
     * Bad startingFromKeys.
     ****************************************************************************/

    // startingFromKey of MaxKey.
    response = shardAdmin.runCommand({cleanupOrphaned: ns, startingFromKey: {_id: MaxKey}});
    assert.commandWorked(response);
    assert.eq(null, response.stoppedAtKey);

    // startingFromKey doesn't match number of fields in shard key.
    assert.commandFailed(shardAdmin.runCommand(
        {cleanupOrphaned: ns, startingFromKey: {someKey: 'someValue', someOtherKey: 1}}));

    // startingFromKey matches number of fields in shard key but not field names.
    assert.commandFailed(
        shardAdmin.runCommand({cleanupOrphaned: ns, startingFromKey: {someKey: 'someValue'}}));

    var coll2 = mongos.getCollection('foo.baz');

    assert.commandWorked(
        mongosAdmin.runCommand({shardCollection: coll2.getFullName(), key: {a: 1, b: 1}}));

    // startingFromKey doesn't match number of fields in shard key.
    assert.commandFailed(shardAdmin.runCommand(
        {cleanupOrphaned: coll2.getFullName(), startingFromKey: {someKey: 'someValue'}}));

    // startingFromKey matches number of fields in shard key but not field names.
    assert.commandFailed(shardAdmin.runCommand(
        {cleanupOrphaned: coll2.getFullName(), startingFromKey: {a: 'someValue', c: 1}}));

    st.stop();

})();
