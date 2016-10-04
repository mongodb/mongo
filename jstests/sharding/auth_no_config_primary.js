/**
 * Tests authorization when a config server has no primary.
 *
 * This test cannot be run on ephemeral storage engines because it requires the users to persist
 * across a restart.
 * @tags: [requires_persistence]
 */
(function() {
    'use strict';

    var st = new ShardingTest({shards: 1, other: {keyFile: 'jstests/libs/key1'}});

    st.s.getDB('admin').createUser({user: 'root', pwd: 'pass', roles: ['root']});
    st.s.getDB('admin').auth('root', 'pass');
    var testDB = st.s.getDB('test');
    testDB.user.insert({hello: 'world'});

    // Kill all secondaries, forcing the current primary to step down.
    st.configRS.getSecondaries().forEach(function(secondaryConn) {
        MongoRunner.stopMongod(secondaryConn.port);
    });

    // Test authenticate through a fresh connection.
    var newConn = new Mongo(st.s.host);

    assert.commandFailedWithCode(newConn.getDB('test').runCommand({find: 'user'}),
                                 ErrorCodes.Unauthorized);

    newConn.getDB('admin').auth('root', 'pass');

    var res = newConn.getDB('test').user.findOne();
    assert.neq(null, res);
    assert.eq('world', res.hello);

    // Test authenticate through new mongos.
    var otherMongos =
        MongoRunner.runMongos({keyFile: "jstests/libs/key1", configdb: st.s.savedOptions.configdb});

    assert.commandFailedWithCode(otherMongos.getDB('test').runCommand({find: 'user'}),
                                 ErrorCodes.Unauthorized);

    otherMongos.getDB('admin').auth('root', 'pass');

    var res = otherMongos.getDB('test').user.findOne();
    assert.neq(null, res);
    assert.eq('world', res.hello);

    st.stop();
})();
