/**
 * Tests that a $lookup and $graphLookup stage within an aggregation pipeline will read only
 * committed data if the pipeline is using a majority readConcern.
 */

load("jstests/replsets/rslib.js");           // For startSetIfSupportsReadMajority.
load("jstests/libs/read_committed_lib.js");  // For testReadCommittedLookup

(function() {

    // Manually create a shard.
    const replSetName = "lookup_read_majority";
    let rst = new ReplSetTest({
        nodes: 3,
        name: replSetName,
        nodeOptions: {
            enableMajorityReadConcern: "",
            shardsvr: "",
        }
    });

    if (!startSetIfSupportsReadMajority(rst)) {
        jsTest.log("skipping test since storage engine doesn't support committed reads");
        return;
    }

    const nodes = rst.nodeList();
    const config = {
        _id: replSetName,
        members: [
            {_id: 0, host: nodes[0]},
            {_id: 1, host: nodes[1], priority: 0},
            {_id: 2, host: nodes[2], arbiterOnly: true},
        ]
    };

    rst.initiate(config);

    let shardSecondary = rst.liveNodes.slaves[0];

    // Confirm read committed works on a cluster with a database that is not sharding enabled.
    let st = new ShardingTest({
        manualAddShard: true,
    });
    assert.commandWorked(st.s.adminCommand({addShard: rst.getURL()}));
    testReadCommittedLookup(st.s.getDB("test"), shardSecondary, rst);

    // Confirm read committed works on a cluster with:
    // - A sharding enabled database
    // - An unsharded local collection
    assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));
    testReadCommittedLookup(st.s.getDB("test"), shardSecondary, rst);

    // Confirm read committed works on a cluster with:
    // - A sharding enabled database
    // - A sharded local collection.
    assert.commandWorked(st.s.getDB("test").runCommand(
        {createIndexes: 'local', indexes: [{name: "foreignKey_1", key: {foreignKey: 1}}]}));
    assert.commandWorked(st.s.adminCommand({shardCollection: 'test.local', key: {foreignKey: 1}}));
    testReadCommittedLookup(st.s.getDB("test"), shardSecondary, rst);

    st.stop();

})();
