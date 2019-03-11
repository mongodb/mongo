//
// Testing migrations between latest and last-stable mongod versions, where the
// donor is the latest version and the recipient the last-stable, and vice versa.
// Migrations should be successful.
//

// Checking UUID consistency involves talking to a shard node, which in this test is shutdown
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {
    "use strict";

    var mixedShard = new ReplSetTest({
        name: 'MixedVersionShard',
        nodes: [
            {binVersion: "last-stable", rsConfig: {votes: 1}},
            {binVersion: "latest", rsConfig: {priority: 0, votes: 0}}
        ]
    });
    mixedShard.startSet();
    mixedShard.initiate();
    mixedShard.restart(0, {'shardsvr': ""});
    mixedShard.restart(1, {'shardsvr': ""});

    var st = new ShardingTest(
        {mongosOptions: {binVersion: "last-stable"}, shards: 0, manualAddShard: true});
    assert.commandWorked(st.addShard(mixedShard.getURL()));

    // Ensure the secondary node also knows that it is sharded by waiting for the identity document
    // write to propagate
    mixedShard.awaitReplication();

    assert.commandWorked(st.s0.adminCommand({enableSharding: 'TestDB'}));
    assert.commandWorked(st.s0.adminCommand({shardCollection: 'TestDB.TestColl', key: {x: 1}}));

    let db = st.s0.getDB('TestDB');
    assert.commandWorked(db.runCommand(
        {find: 'TestColl', $readPreference: {mode: 'secondary'}, readConcern: {'level': 'local'}}));

    st.stop();
    mixedShard.stopSet();
})();
