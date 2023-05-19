/**
 * This test serves as a baseline for measuring the scalability of the ShardingTest fixture.
 *
 * It allows us to compare the performance of the setup and teardown procedures of ShardingTest with
 * a single node against the performance with many nodes, to see how the fixture scales. See the
 * 'shardingtest_control_12_nodes.js' test.
 */
(function() {
const st = new ShardingTest({shards: 1, rs: {nodes: 1}, mongos: 1});
st.stop();
})();
