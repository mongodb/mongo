/**
 * This test serves as a baseline for measuring the scalability of the ReplSetTest fixture.
 *
 * It allows us to compare the performance of the setup and teardown procedures of ReplSetTest with
 * a single node against the performance with many nodes, to see how the fixture scales. See the
 * 'replsettest_control_12_nodes.js' test.
 */
(function() {
const replTest = new ReplSetTest({name: 'replsettest_control_1_node', nodes: 1});
replTest.startSet();
replTest.initiate();
replTest.stopSet();
})();
