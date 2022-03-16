/**
 * Verify the "cluster" versions of commands can only run on a sharding enabled shardsvr mongod.
 *
 * @tags: [
 *   requires_replication,
 *   requires_sharding,
 * ]
 */
(function() {
"use strict";

const kDBName = "foo";
const kCollName = "bar";

//
// Standalone mongods have cluster commands, but they cannot be run.
//
{
    const standalone = MongoRunner.runMongod({});
    assert(standalone);

    assert.commandFailedWithCode(standalone.getDB(kDBName).runCommand({clusterFind: kCollName}),
                                 ErrorCodes.ShardingStateNotInitialized);

    MongoRunner.stopMongod(standalone);
}

//
// Standalone replica sets mongods have cluster commands, but they cannot be run.
//
{
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    assert.commandFailedWithCode(
        rst.getPrimary().getDB(kDBName).runCommand({clusterFind: kCollName}),
        ErrorCodes.ShardingStateNotInitialized);

    rst.stopSet();
}

//
// Cluster commands exist on shardsvrs but require sharding to be enabled.
//
{
    const shardsvrRst = new ReplSetTest({nodes: 1});
    shardsvrRst.startSet({shardsvr: ""});
    shardsvrRst.initiate();

    assert.commandFailedWithCode(
        shardsvrRst.getPrimary().getDB(kDBName).runCommand({clusterFind: kCollName}),
        ErrorCodes.ShardingStateNotInitialized);

    shardsvrRst.stopSet();
}

{
    const st = new ShardingTest({mongos: 1, shards: 1, config: 1});

    //
    // Cluster commands do not exist on mongos.
    //

    assert.commandFailedWithCode(st.s.getDB(kDBName).runCommand({clusterFind: kCollName}),
                                 ErrorCodes.CommandNotFound);

    //
    // Cluster commands work on sharding enabled shardsvr.
    //

    assert.commandWorked(st.rs0.getPrimary().getDB(kDBName).runCommand({clusterFind: kCollName}));

    //
    // Cluster commands work on config server.
    //

    assert.commandWorked(
        st.configRS.getPrimary().getDB(kDBName).runCommand({clusterFind: kCollName}));

    st.stop();
}
}());
