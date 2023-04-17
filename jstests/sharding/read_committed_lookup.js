/**
 * Tests that a $lookup and $graphLookup stage within an aggregation pipeline will read only
 * committed data if the pipeline is using a majority readConcern.
 *
 * @tags: [requires_majority_read_concern]
 */

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

let shardSecondary = rst.getSecondary();

// Confirm read committed works on a cluster with a database that is not sharding enabled.
let st = new ShardingTest({
    manualAddShard: true,
});
if (TestData.configShard) {
    assert.commandWorked(st.s.adminCommand({transitionFromDedicatedConfigServer: 1}));
}
// The default WC is majority and this test can't satisfy majority writes.
assert.commandWorked(st.s.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));

// Even though implicitDefaultWC is set to w:1, addShard will work as CWWC is set.
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
rst.stopSet();
})();
