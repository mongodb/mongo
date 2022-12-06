/**
 * Checks that setClusterParameter behaves as expected during restart.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   # Restarts all replica set member nodes mid-test.
 *   requires_persistence,
 *   requires_replication,
 *   requires_fcv_62,
 *   serverless
 *  ]
 */
(function() {
'use strict';

load('jstests/libs/cluster_server_parameter_utils.js');

const tenantId = ObjectId();

let intParameter1 = {
    _id: "testIntClusterParameter",
};
let strParameter1 = {
    _id: "testStrClusterParameter",
};
let intParameter2 = {
    _id: "testIntClusterParameter",
};
let strParameter2 = {
    _id: "testStrClusterParameter",
};

function setParamsAndValidateCluster(rst) {
    runSetClusterParameter(rst.getPrimary(), intParameter1);
    runSetClusterParameter(rst.getPrimary(), strParameter1);
    runSetClusterParameter(rst.getPrimary(), intParameter2, tenantId);
    runSetClusterParameter(rst.getPrimary(), strParameter2, tenantId);

    // Check that the new values are visible on the majority of the nodes.
    runGetClusterParameterReplicaSet(rst,
                                     ["testIntClusterParameter", "testStrClusterParameter"],
                                     [intParameter1, strParameter1]);
    runGetClusterParameterReplicaSet(rst,
                                     ["testIntClusterParameter", "testStrClusterParameter"],
                                     [intParameter2, strParameter2],
                                     tenantId);
}
// Checks that up-to-date cluster parameters are transferred over to newly-added replica set nodes
// as part of initial sync.
function checkClusterParameterInitialSync(rst, newNodeOptions) {
    // Update some parameter values.
    intParameter1.intData = 5;
    strParameter1.strData = "state 1";
    intParameter2.intData = 6;
    strParameter2.strData = "state 2";
    setParamsAndValidateCluster(rst);

    // Add a new node to the replica set, reinitiate the set, and wait for it to become a secondary
    // with all data fully replicated to it.
    const newNode = rst.add(newNodeOptions);
    rst.reInitiate();
    rst.waitForState(newNode, ReplSetTest.State.SECONDARY);
    rst.awaitReplication();

    // Check that the new node has the latest cluster parameter values.
    assert(runGetClusterParameterNode(newNode,
                                      ["testIntClusterParameter", "testStrClusterParameter"],
                                      [intParameter1, strParameter1]));
    assert(runGetClusterParameterNode(newNode,
                                      ["testIntClusterParameter", "testStrClusterParameter"],
                                      [intParameter2, strParameter2],
                                      tenantId));

    // Check that setClusterParameter properly works with the reconfigured replica set.
    intParameter1.intData = 30;
    strParameter1.strData = "sleep";
    intParameter2.intData = 31;
    strParameter2.strData = "wake";
    setParamsAndValidateCluster(rst);
}

// Checks that restarted replica sets start with the most recent majority-written cluster parameter
// values.
function checkClusterParameterRestart(rst) {
    // Update some parameter values.
    intParameter1.intData = 7;
    strParameter1.strData = "state 3";
    intParameter2.intData = 8;
    strParameter2.strData = "state 4";
    setParamsAndValidateCluster(rst);

    // Bounce restart all of the nodes.
    rst.nodeList().forEach((_, index) => {
        rst.restart(index);
    });

    // Check that restarted replica set still has the most recent setClusterParameter values.
    runGetClusterParameterReplicaSet(rst,
                                     ["testIntClusterParameter", "testStrClusterParameter"],
                                     [intParameter1, strParameter1]);
    runGetClusterParameterReplicaSet(rst,
                                     ["testIntClusterParameter", "testStrClusterParameter"],
                                     [intParameter2, strParameter2],
                                     tenantId);
}

const baseOptions = {
    setParameter: {multitenancySupport: true, featureFlagRequireTenantID: true}
};

const rst = new ReplSetTest({
    nodes: 3,
    serverless: true,
});
rst.startSet(baseOptions);
rst.initiate();

for (let syncMethod of ["logical", "fileCopyBased"]) {
    jsTest.log("Testing initial sync w/ method = " + syncMethod);
    let options = baseOptions;
    options.setParameter.initialSyncMethod = syncMethod;
    checkClusterParameterInitialSync(rst, options);
}

jsTest.log("Testing cluster restart");
checkClusterParameterRestart(rst);

rst.stopSet();
})();
