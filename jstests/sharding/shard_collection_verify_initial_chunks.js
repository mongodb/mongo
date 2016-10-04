//
// Verify numInitialChunks can not be set for non hashed key or nonempty collections
//

(function() {
    'use strict';

    var st = new ShardingTest({mongos: 1, shards: 2});
    var kDbName = 'db';
    var mongos = st.s0;

    assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));

    assert.commandFailed(mongos.adminCommand(
        {shardCollection: kDbName + '.foo', key: {aKey: 1}, numInitialChunks: 5}));

    assert.writeOK(mongos.getDB(kDbName).foo.insert({aKey: 1}));
    assert.commandWorked(mongos.getDB(kDbName).foo.createIndex({aKey: "hashed"}));
    assert.commandFailed(mongos.adminCommand(
        {shardCollection: kDbName + '.foo', key: {aKey: "hashed"}, numInitialChunks: 5}));

    assert.writeOK(mongos.getDB(kDbName).foo.remove({}));
    assert.commandWorked(mongos.adminCommand(
        {shardCollection: kDbName + '.foo', key: {aKey: "hashed"}, numInitialChunks: 5}));

    mongos.getDB(kDbName).dropDatabase();

    st.stop();

})();
