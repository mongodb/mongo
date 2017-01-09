load('jstests/replsets/rslib.js');
(function() {
    "use strict";

    var st = new ShardingTest({
        name: "zzz",
        shards: {
            rs0: {
                nodes: {n0: {}, n1: {rsConfig: {priority: 0}}},
                oplogSize: 10,
            }
        },
    });
    var replTest = st.rs0;

    var config = replTest.getReplSetConfig();
    // Add a delay long enough so getLastError would actually 'wait' for write concern.
    config.members[1].slaveDelay = 3;
    config.version = replTest.getReplSetConfigFromNode().version + 1;

    reconfig(replTest, config, true);

    assert.soon(function() {
        var secConn = replTest.getSecondary();
        var config = secConn.getDB('local').system.replset.findOne();
        return config.members[1].slaveDelay == 3;
    });

    replTest.awaitSecondaryNodes();

    var testDB = st.s.getDB('test');
    testDB.adminCommand({connPoolSync: 1});

    var secConn = replTest.getSecondary();
    var testDB2 = secConn.getDB('test');

    testDB.user.insert({x: 1});

    testDB.user.ensureIndex({x: 1});
    assert.gleOK(testDB.runCommand({getLastError: 1, w: 2}));

    var priIdx = testDB.user.getIndexes();
    var secIdx = testDB2.user.getIndexes();

    assert.eq(priIdx.length, secIdx.length, 'pri: ' + tojson(priIdx) + ', sec: ' + tojson(secIdx));

    st.stop();

}());
