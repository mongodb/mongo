/**
 * Checks that set/getClusterParameter omit test-only parameters when enableTestCommands
 * is false.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   requires_replication,
 *   requires_fcv_62,
 *   serverless
 *  ]
 */
(function() {
'use strict';

load('jstests/libs/cluster_server_parameter_utils.js');

// Verifies that test-only parameters are disabled and excluded when enableTestCommands is false.
TestData.enableTestCommands = false;
const rst = new ReplSetTest({
    nodes: 3,
});
rst.startSet({
    setParameter:
        {multitenancySupport: true, featureFlagMongoStore: true, featureFlagRequireTenantID: true}
});
rst.initiate();

// Setup the necessary logging level for the test.
setupReplicaSet(rst);

// Assert that turning off enableTestCommands prevents test-only cluster server parameters
// from being directly set/retrieved and filters them from the output of
// getClusterParameter: '*' with and without a tenantId.
testDisabledClusterParameters(rst);
testDisabledClusterParameters(rst, ObjectId());
rst.stopSet();
}());
