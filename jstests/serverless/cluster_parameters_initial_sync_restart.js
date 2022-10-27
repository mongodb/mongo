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

// Checks that restarted replica sets start with the most recent majority-written cluster parameter
// values.
function checkClusterParameterRestart(rst) {
    // Update some parameter values.
    let newIntParameter1 = {
        _id: "testIntClusterParameter",
        intData: 7,
    };
    let newStrParameter1 = {
        _id: "testStrClusterParameter",
        strData: "state 3",
    };
    let newIntParameter2 = {
        _id: "testIntClusterParameter",
        intData: 8,
    };
    let newStrParameter2 = {
        _id: "testStrClusterParameter",
        strData: "state 4",
    };
    runSetClusterParameter(rst.getPrimary(), newIntParameter1);
    runSetClusterParameter(rst.getPrimary(), newStrParameter1);
    runSetClusterParameter(rst.getPrimary(), newIntParameter2, tenantId);
    runSetClusterParameter(rst.getPrimary(), newStrParameter2, tenantId);

    // Check that the new values are visible on the majority of the nodes.
    runGetClusterParameterReplicaSet(rst,
                                     ["testIntClusterParameter", "testStrClusterParameter"],
                                     [newIntParameter1, newStrParameter1]);
    runGetClusterParameterReplicaSet(rst,
                                     ["testIntClusterParameter", "testStrClusterParameter"],
                                     [newIntParameter2, newStrParameter2],
                                     tenantId);

    // Bounce restart all of the nodes.
    rst.nodeList().forEach((_, index) => {
        rst.restart(index);
    });

    // Check that restarted replica set still has the most recent setClusterParameter values.
    runGetClusterParameterReplicaSet(rst,
                                     ["testIntClusterParameter", "testStrClusterParameter"],
                                     [newIntParameter1, newStrParameter1]);
    runGetClusterParameterReplicaSet(rst,
                                     ["testIntClusterParameter", "testStrClusterParameter"],
                                     [newIntParameter2, newStrParameter2],
                                     tenantId);
}

const rst = new ReplSetTest({
    nodes: 3,
});
rst.startSet({
    setParameter:
        {multitenancySupport: true, featureFlagMongoStore: true, featureFlagRequireTenantID: true}
});
rst.initiate();

checkClusterParameterRestart(rst);

rst.stopSet();
})();
