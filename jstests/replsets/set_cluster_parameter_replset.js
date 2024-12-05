/**
 * Checks that setClusterParameter behaves as expected during initial sync and restart.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   # Restarts all replica set member nodes mid-test.
 *   requires_persistence,
 *  ]
 */
import {
    runGetClusterParameterNode,
    runGetClusterParameterReplicaSet,
    runSetClusterParameter,
} from "jstests/libs/cluster_server_parameter_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Checks that up-to-date cluster parameters are transferred over to newly-added replica set nodes
// as part of initial sync.
function checkClusterParameterInitialSync(rst) {
    // Update some parameter values.
    let newIntParameter = {
        _id: "testIntClusterParameter",
        intData: 5,
    };
    let newStrParameter = {
        _id: "testStrClusterParameter",
        strData: "on",
    };
    runSetClusterParameter(rst.getPrimary(), newIntParameter);
    runSetClusterParameter(rst.getPrimary(), newStrParameter);

    // Check that the new values are visible on the majority of the nodes.
    rst.awaitReplication();
    runGetClusterParameterReplicaSet(rst,
                                     ["testIntClusterParameter", "testStrClusterParameter"],
                                     [newIntParameter, newStrParameter]);

    // Add a new node to the replica set, reinitiate the set, and wait for it to become a secondary
    // with all data fully replicated to it.
    const newNode = rst.add({});
    rst.reInitiate();
    rst.awaitSecondaryNodes(null, [newNode]);
    rst.awaitReplication();

    // Check that the new node has the latest cluster parameter values.
    assert(runGetClusterParameterNode(newNode,
                                      ["testIntClusterParameter", "testStrClusterParameter"],
                                      [newIntParameter, newStrParameter]));

    // Check that setClusterParameter properly works with the reconfigured replica set.
    newIntParameter.intData = 30;
    newStrParameter.strData = "sleep";
    runSetClusterParameter(rst.getPrimary(), newIntParameter);
    runSetClusterParameter(rst.getPrimary(), newStrParameter);

    // Check that the new values are visible on the majority of the nodes.
    runGetClusterParameterReplicaSet(rst,
                                     ["testIntClusterParameter", "testStrClusterParameter"],
                                     [newIntParameter, newStrParameter]);
}

// Checks that restarted replica sets start with the most recent majority-written cluster parameter
// values.
function checkClusterParameterRestart(rst) {
    // Update some parameter values.
    let newIntParameter = {
        _id: "testIntClusterParameter",
        intData: 8,
    };
    let newStrParameter = {
        _id: "testStrClusterParameter",
        strData: "dormant",
    };
    runSetClusterParameter(rst.getPrimary(), newIntParameter);
    runSetClusterParameter(rst.getPrimary(), newStrParameter);

    // Check that the new values are visible on the majority of the nodes.
    rst.awaitReplication();
    runGetClusterParameterReplicaSet(rst,
                                     ["testIntClusterParameter", "testStrClusterParameter"],
                                     [newIntParameter, newStrParameter]);

    // Bounce restart all of the nodes.
    rst.nodeList().forEach((_, index) => {
        rst.restart(index);
    });

    // Check that restarted replica set still has the most recent setClusterParameter values.
    runGetClusterParameterReplicaSet(rst,
                                     ["testIntClusterParameter", "testStrClusterParameter"],
                                     [newIntParameter, newStrParameter]);
}

const rst = new ReplSetTest({
    nodes: 2,
});
rst.startSet();
rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

checkClusterParameterInitialSync(rst);
checkClusterParameterRestart(rst);

rst.stopSet();
