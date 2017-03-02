//
// Verify numInitialChunks can not be set for non hashed key or nonempty collections
//

(function() {
    'use strict';

    var st = new ShardingTest({bongos: 1, shards: 2});
    var kDbName = 'db';
    var bongos = st.s0;

    assert.commandWorked(bongos.adminCommand({enableSharding: kDbName}));

    assert.commandFailed(bongos.adminCommand(
        {shardCollection: kDbName + '.foo', key: {aKey: 1}, numInitialChunks: 5}));

    assert.writeOK(bongos.getDB(kDbName).foo.insert({aKey: 1}));
    assert.commandWorked(bongos.getDB(kDbName).foo.createIndex({aKey: "hashed"}));
    assert.commandFailed(bongos.adminCommand(
        {shardCollection: kDbName + '.foo', key: {aKey: "hashed"}, numInitialChunks: 5}));

    assert.writeOK(bongos.getDB(kDbName).foo.remove({}));
    assert.commandWorked(bongos.adminCommand(
        {shardCollection: kDbName + '.foo', key: {aKey: "hashed"}, numInitialChunks: 5}));

    bongos.getDB(kDbName).dropDatabase();

    st.stop();

})();
