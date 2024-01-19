import {MetadataConsistencyChecker} from "jstests/libs/check_metadata_consistency_helpers.js";
import {DiscoverTopology, Topology} from "jstests/libs/discover_topology.js";

assert.neq(typeof db, 'undefined', 'No `db` object, is the shell connected to a server?');

const conn = db.getMongo();

{
    // Check that we are running on a sharded cluster
    let topology;
    try {
        topology = DiscoverTopology.findConnectedNodes(conn);
    } catch (e) {
        if (ErrorCodes.isRetriableError(e.code) || ErrorCodes.isInterruption(e.code) ||
            ErrorCodes.isNetworkTimeoutError(e.code)) {
            jsTest.log(
                `Aborted metadata consistency check due to retriable error during topology discovery: ${
                    e}`);
            quit();
        } else {
            throw e;
        }
    }
    assert(topology.type == Topology.kShardedCluster ||
               (topology.type == Topology.kReplicaSet && topology.configsvr &&
                TestData.testingReplicaSetEndpoint),
           "Metadata consistency check must be run against a sharded cluster");
}

MetadataConsistencyChecker.run(conn);
