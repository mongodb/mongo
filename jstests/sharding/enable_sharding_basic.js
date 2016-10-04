//
// Basic tests for enableSharding command.
//

(function() {
    'use strict';

    var st = new ShardingTest({mongos: 1, shards: 2});

    var mongos = st.s0;

    // enableSharing can run only on mongos.
    assert.commandFailedWithCode(st.d0.getDB('admin').runCommand({enableSharding: 'db'}),
                                 ErrorCodes.CommandNotFound);

    // enableSharing can run only against the admin database.
    assert.commandFailedWithCode(mongos.getDB('test').runCommand({enableSharding: 'db'}),
                                 ErrorCodes.Unauthorized);

    // Can't shard 'config' database.
    assert.commandFailed(mongos.adminCommand({enableSharding: 'config'}));

    // Can't shard 'local' database.
    assert.commandFailed(mongos.adminCommand({enableSharding: 'local'}));

    // Can't shard 'admin' database.
    assert.commandFailed(mongos.adminCommand({enableSharding: 'admin'}));

    // Can't shard db with the name that just differ on case.
    assert.commandWorked(mongos.adminCommand({enableSharding: 'db'}));
    assert.eq(mongos.getDB('config').databases.findOne({_id: 'db'}).partitioned, true);

    assert.commandFailedWithCode(mongos.adminCommand({enableSharding: 'DB'}),
                                 ErrorCodes.DatabaseDifferCase);

    // Can't shard invalid db name.
    assert.commandFailed(mongos.adminCommand({enableSharding: 'a.b'}));
    assert.commandFailed(mongos.adminCommand({enableSharding: ''}));

    // Can't shard already sharded database.
    assert.commandFailedWithCode(mongos.adminCommand({enableSharding: 'db'}),
                                 ErrorCodes.AlreadyInitialized);
    assert.eq(mongos.getDB('config').databases.findOne({_id: 'db'}).partitioned, true);

    // Verify config.databases metadata.
    assert.writeOK(mongos.getDB('unsharded').foo.insert({aKey: "aValue"}));
    assert.eq(mongos.getDB('config').databases.findOne({_id: 'unsharded'}).partitioned, false);
    assert.commandWorked(mongos.adminCommand({enableSharding: 'unsharded'}));
    assert.eq(mongos.getDB('config').databases.findOne({_id: 'unsharded'}).partitioned, true);

    st.stop();

})();
