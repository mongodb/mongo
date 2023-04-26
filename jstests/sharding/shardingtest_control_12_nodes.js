/**
 * This test serves as a baseline for measuring the scalability of the ShardingTest fixture.
 *
 * It allows us to compare the performance of the setup and teardown procedures of ShardingTest with
 * many nodes against the performance with a single node, to see how the fixture scales. See the
 * 'shardingtest_control_1_node.js' test. In particular, we want to be able to see if ShardingTest
 * parallelizes its setup and teardown procedures well.
 *
 * We use 12 total shard replica set nodes because we consider that to be a reasonable scalability
 * limit for ShardingTest. We expect the large majority of tests will never use more than that
 * number of nodes, and so we're not particularly worried about scaling beyond that point.
 */
(function() {
const st = new ShardingTest({shards: 4, rs: {nodes: 3}, mongos: 1});
st.stop();
})();
