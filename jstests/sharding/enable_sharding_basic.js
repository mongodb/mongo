//
// Basic tests for enableSharding command.
//

(function() {
    'use strict';

    var st = new ShardingTest({bongos: 1, shards: 2});

    var bongos = st.s0;

    // enableSharing can run only on bongos.
    assert.commandFailedWithCode(st.d0.getDB('admin').runCommand({enableSharding: 'db'}),
                                 ErrorCodes.CommandNotFound);

    // enableSharing can run only against the admin database.
    assert.commandFailedWithCode(bongos.getDB('test').runCommand({enableSharding: 'db'}),
                                 ErrorCodes.Unauthorized);

    // Can't shard 'config' database.
    assert.commandFailed(bongos.adminCommand({enableSharding: 'config'}));

    // Can't shard 'local' database.
    assert.commandFailed(bongos.adminCommand({enableSharding: 'local'}));

    // Can't shard 'admin' database.
    assert.commandFailed(bongos.adminCommand({enableSharding: 'admin'}));

    // Can't shard db with the name that just differ on case.
    assert.commandWorked(bongos.adminCommand({enableSharding: 'db'}));
    assert.eq(bongos.getDB('config').databases.findOne({_id: 'db'}).partitioned, true);

    assert.commandFailedWithCode(bongos.adminCommand({enableSharding: 'DB'}),
                                 ErrorCodes.DatabaseDifferCase);

    // Can't shard invalid db name.
    assert.commandFailed(bongos.adminCommand({enableSharding: 'a.b'}));
    assert.commandFailed(bongos.adminCommand({enableSharding: ''}));

    // Can't shard already sharded database.
    assert.commandFailedWithCode(bongos.adminCommand({enableSharding: 'db'}),
                                 ErrorCodes.AlreadyInitialized);
    assert.eq(bongos.getDB('config').databases.findOne({_id: 'db'}).partitioned, true);

    // Verify config.databases metadata.
    assert.writeOK(bongos.getDB('unsharded').foo.insert({aKey: "aValue"}));
    assert.eq(bongos.getDB('config').databases.findOne({_id: 'unsharded'}).partitioned, false);
    assert.commandWorked(bongos.adminCommand({enableSharding: 'unsharded'}));
    assert.eq(bongos.getDB('config').databases.findOne({_id: 'unsharded'}).partitioned, true);

    st.stop();

})();
