/**
 * Checks that set/getClusterParameter run as expected on standalone.
 * @tags: [
 *   # Standalone cluster parameters enabled only under this flag.
 *   featureFlagAuditConfigClusterParameter,
 * ]
 */
(function() {
'use strict';

load('jstests/libs/cluster_server_parameter_utils.js');

const conn = MongoRunner.runMongod({});

// Setup the necessary logging level for the test.
setupNode(conn);

// First, ensure that incorrect usages of set/getClusterParameter fail appropriately.
testInvalidClusterParameterCommands(conn);

// Then, ensure that set/getClusterParameter set and retrieve expected values.
testValidClusterParameterCommands(conn);

MongoRunner.stopMongod(conn);
})();
