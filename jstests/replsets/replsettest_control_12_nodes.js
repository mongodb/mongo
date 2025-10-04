/**
 * This test serves as a baseline for measuring the scalability of the ReplSetTest fixture.
 *
 * It allows us to compare the performance of the setup and teardown procedures of ReplSetTest with
 * many nodes against the performance with a single node, to see how the fixture scales. See the
 * 'replsettest_control_1_node.js' test. In particular, we want to be able to know if ReplSetTest
 * parallelizes its setup and teardown procedures well.
 *
 * We use 12 replica set nodes because we consider that to be a reasonable scalability limit for
 * ReplSetTest. We expect the large majority of tests will never use more than this number of nodes,
 * and so we're not particularly worried about scaling beyond that point.
 *
 * We disable the test on windows because it can cause evergreen timeouts on slow machines in the
 * debug variant. The test will still run locally so we don't lose test coverage this way.
 *
 * @tags: [resource_intensive, incompatible_with_windows_tls]
 *
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Add replication-level logging.
TestData.setParameters = TestData.setParameters || {};
TestData.setParameters.logComponentVerbosity = TestData.setParameters.logComponentVerbosity || {};
TestData.setParameters.logComponentVerbosity.replication =
    TestData.setParameters.logComponentVerbosity.replication || {};
TestData.setParameters.logComponentVerbosity.replication = Object.merge(
    TestData.setParameters.logComponentVerbosity.replication,
    {verbosity: 2},
);

// There are a limited number of voting nodes allowed in a replica set. We use as many voting nodes
// as possible and fill in the rest with non-voting nodes.
const numNodes = 12;
const maxNumVotingNodes = 7;
let allNodes = [];
for (let i = 0; i < numNodes; i++) {
    allNodes.push(i < maxNumVotingNodes ? {} : {rsConfig: {votes: 0, priority: 0}});
}
const replTest = new ReplSetTest({name: "replsettest_control_12_nodes", nodes: allNodes});
replTest.startSet();
replTest.initiate();
replTest.stopSet();
