/**
 * Checks that setClusterParameter behaves as expected during restart.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   # Restarts all replica set member nodes mid-test.
 *   requires_persistence,
 *   requires_replication,
 *   requires_fcv_62,
 *  ]
 */
import {
    runGetClusterParameterNode,
    runGetClusterParameterReplicaSet,
    runSetClusterParameter,
} from "jstests/libs/cluster_server_parameter_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

let intParameter = {
    _id: "testIntClusterParameter",
};
let strParameter = {
    _id: "testStrClusterParameter",
};

function setParamsAndValidateCluster(rst) {
    runSetClusterParameter(rst.getPrimary(), intParameter);
    runSetClusterParameter(rst.getPrimary(), strParameter);

    // Check that the new values are visible on the majority of the nodes.
    runGetClusterParameterReplicaSet(
        rst,
        ["testIntClusterParameter", "testStrClusterParameter"],
        [intParameter, strParameter],
    );
}
// Checks that up-to-date cluster parameters are transferred over to newly-added replica set nodes
// as part of initial sync.
function checkClusterParameterInitialSync(rst, newNodeOptions) {
    // Update some parameter values.
    intParameter.intData = 5;
    strParameter.strData = "state 1";
    setParamsAndValidateCluster(rst);

    // Add a new node to the replica set, reinitiate the set, and wait for it to become a secondary
    // with all data fully replicated to it.
    const newNode = rst.add(newNodeOptions);
    rst.waitForAllNewlyAddedRemovals();
    rst.reInitiate();
    rst.awaitSecondaryNodes(null, [newNode]);
    rst.awaitReplication();

    // Check that the new node has the latest cluster parameter values.
    assert(
        runGetClusterParameterNode(
            newNode,
            ["testIntClusterParameter", "testStrClusterParameter"],
            [intParameter, strParameter],
        ),
    );

    // Check that setClusterParameter properly works with the reconfigured replica set.
    intParameter.intData = 30;
    strParameter.strData = "sleep";
    setParamsAndValidateCluster(rst);
}

// Checks that restarted replica sets start with the most recent majority-written cluster parameter
// values.
function checkClusterParameterRestart(rst) {
    // Update some parameter values.
    intParameter.intData = 7;
    strParameter.strData = "state 2";
    setParamsAndValidateCluster(rst);

    // Bounce restart all of the nodes.
    rst.nodeList().forEach((_, index) => {
        rst.restart(index);
    });

    // Check that restarted replica set still has the most recent setClusterParameter values.
    runGetClusterParameterReplicaSet(
        rst,
        ["testIntClusterParameter", "testStrClusterParameter"],
        [intParameter, strParameter],
    );
}

const baseOptions = {
    setParameter: {},
};

const rst = new ReplSetTest({
    nodes: 3,
});
rst.startSet(baseOptions);
rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

for (let syncMethod of ["logical", "fileCopyBased"]) {
    jsTest.log("Testing initial sync w/ method = " + syncMethod);
    let options = baseOptions;
    options.setParameter.initialSyncMethod = syncMethod;
    checkClusterParameterInitialSync(rst, options);
}

jsTest.log("Testing cluster restart");
checkClusterParameterRestart(rst);

rst.stopSet();
