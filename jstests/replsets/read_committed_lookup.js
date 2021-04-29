/**
 * Tests that a $lookup and $graphLookup stage within an aggregation pipeline will read only
 * committed data if the pipeline is using a majority readConcern.
 *
 * @tags: [requires_majority_read_concern]
 */

load("jstests/libs/read_committed_lib.js");  // For testReadCommittedLookup

(function() {
"use strict";

// Confirm majority readConcern works on a replica set.
const replSetName = "lookup_read_majority";
let rst = new ReplSetTest({
    nodes: 3,
    name: replSetName,
    nodeOptions: {
        enableMajorityReadConcern: "",
        shardsvr: "",
    }
});

rst.startSet();

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

let st = new ShardingTest({
    manualAddShard: true,
});
assert.commandWorked(st.s.adminCommand({addShard: rst.getURL()}));
// The default WC is majority and stopServerReplication will prevent satisfying any majority writes.
// We can't run this command on a shard server (configured with --shardsvr) which is why we must run
// it on mongos.
assert.commandWorked(st.s.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));

let shardSecondary = rst.getSecondary();

testReadCommittedLookup(rst.getPrimary().getDB("test"), shardSecondary, rst);

st.stop();
rst.stopSet();
})();
