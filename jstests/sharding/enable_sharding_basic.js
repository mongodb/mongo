//
// Basic tests for enableSharding command.
//

(function() {
    'use strict';

    var st = new ShardingTest({mongos: 2, shards: 2});

    // enableSharding can run only on mongos.
    assert.commandFailedWithCode(st.d0.getDB('admin').runCommand({enableSharding: 'db'}),
                                 ErrorCodes.CommandNotFound);

    // enableSharding can run only against the admin database.
    assert.commandFailedWithCode(st.s0.getDB('test').runCommand({enableSharding: 'db'}),
                                 ErrorCodes.Unauthorized);

    // Can't shard 'local' database.
    assert.commandFailed(st.s0.adminCommand({enableSharding: 'local'}));

    // Can't shard 'admin' database.
    assert.commandFailed(st.s0.adminCommand({enableSharding: 'admin'}));

    // Can't shard db with the name that just differ on case.
    assert.commandWorked(st.s0.adminCommand({enableSharding: 'db'}));
    assert.eq(st.s0.getDB('config').databases.findOne({_id: 'db'}).partitioned, true);

    assert.commandFailedWithCode(st.s0.adminCommand({enableSharding: 'DB'}),
                                 ErrorCodes.DatabaseDifferCase);

    // Can't shard invalid db name.
    assert.commandFailed(st.s0.adminCommand({enableSharding: 'a.b'}));
    assert.commandFailed(st.s0.adminCommand({enableSharding: ''}));

    // Attempting to shard already sharded database returns success.
    assert.commandWorked(st.s0.adminCommand({enableSharding: 'db'}));
    assert.eq(st.s0.getDB('config').databases.findOne({_id: 'db'}).partitioned, true);

    // Verify config.databases metadata.
    assert.writeOK(st.s0.getDB('unsharded').foo.insert({aKey: "aValue"}));
    assert.eq(st.s0.getDB('config').databases.findOne({_id: 'unsharded'}).partitioned, false);
    assert.commandWorked(st.s0.adminCommand({enableSharding: 'unsharded'}));
    assert.eq(st.s0.getDB('config').databases.findOne({_id: 'unsharded'}).partitioned, true);

    // Sharding a collection before 'enableSharding' is called fails
    assert.commandFailed(st.s0.adminCommand({shardCollection: 'TestDB.TestColl', key: {_id: 1}}));
    assert.commandFailed(st.s1.adminCommand({shardCollection: 'TestDB.TestColl', key: {_id: 1}}));

    assert.writeOK(st.s0.getDB('TestDB').TestColl.insert({_id: 0}));
    assert.writeOK(st.s1.getDB('TestDB').TestColl.insert({_id: 1}));

    // Calling 'enableSharding' on one mongos and 'shardCollection' through another must work
    assert.commandWorked(st.s0.adminCommand({enableSharding: 'TestDB'}));
    assert.commandWorked(st.s1.adminCommand({shardCollection: 'TestDB.TestColl', key: {_id: 1}}));
    assert.commandWorked(st.s0.adminCommand({shardCollection: 'TestDB.TestColl', key: {_id: 1}}));

    st.stop();
})();
