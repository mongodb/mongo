'use strict';

/**
 * Executes the create_index_background.js workload, but with a wildcard index.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');               // For extendWorkload.
load('jstests/concurrency/fsm_workloads/create_index_background.js');  // For $config.
load('jstests/libs/discover_topology.js');  // For findDataBearingNodes().

var $config = extendWorkload($config, function($config, $super) {
    $config.data.getIndexSpec = function() {
        return {"$**": 1};
    };

    $config.data.extendDocument = function extendDocument(originalDoc) {
        const fieldName = "arrayField";

        // Be sure we're not overwriting an existing field.
        assertAlways.eq(originalDoc.hasOwnProperty(fieldName), false);

        // Insert a field which has an array as the value, to exercise the special multikey
        // metadata functionality wildcard indexes rely on.
        originalDoc[fieldName] = [1, 2, "string", this.tid];
        return originalDoc;
    };

    $config.setup = function setup() {
        // Enable the test flag required in order to build $** indexes on all data bearing nodes.
        // TODO SERVER-36198: Remove this.
        const hosts = DiscoverTopology.findDataBearingNodes(db.getMongo());
        for (let host of hosts) {
            const conn = new Mongo(host);
            const adminDB = conn.getDB("admin");
            assert.commandWorked(
                adminDB.adminCommand({setParameter: 1, internalQueryAllowAllPathsIndexes: true}));
        }

        $super.setup.apply(this, arguments);
    };

    return $config;
});
