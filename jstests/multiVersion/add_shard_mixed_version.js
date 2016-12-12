/**
 * Tests that you can continue to add previous-version shards throughout the upgrade process, until
 * you start using mongoses from the current version.
 */

(function() {
    "use strict";

    var waitForIsMaster = function(conn) {
        assert.soon(function() {
            var res = conn.getDB('admin').runCommand({isMaster: 1});
            return res.ismaster;
        });
    };

    // Create the cluster to test adding shards to.
    // Config servers and first shard will be current version, mongos will be previous version.
    var st = new ShardingTest({shards: 1, mongos: {s0: {binVersion: 'last-stable'}}});

    jsTest.log(
        "Verify that we can add a previous-version shard through the previous-version mongos.");
    var newShard1 = MongoRunner.runMongod({shardsvr: '', binVersion: 'last-stable'});
    waitForIsMaster(newShard1);

    var newShard1Name = 'newShard1';
    assert.commandWorked(st.s0.adminCommand({addShard: newShard1.host, name: newShard1Name}));

    assert.commandWorked(st.s0.adminCommand({removeShard: newShard1Name}));
    assert.commandWorked(st.s0.adminCommand({removeShard: newShard1Name}));
    MongoRunner.stopMongod(newShard1);

    jsTest.log(
        "Verify that we cannot add a previous-version shard through a current-version mongos.");

    st.restartMongos(0, {restart: true, binVersion: 'latest'});

    var newShard2 = MongoRunner.runMongod({shardsvr: '', binVersion: 'last-stable'});
    waitForIsMaster(newShard2);
    var newShard2Name = 'newShard2';

    assert.commandFailedWithCode(
        st.s0.adminCommand({addShard: newShard2.host, name: newShard2Name}),
        ErrorCodes.IncompatibleServerVersion);

    MongoRunner.stopMongod(newShard2);
    st.stop();

})();
