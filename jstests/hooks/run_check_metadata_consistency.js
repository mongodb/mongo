import {MetadataConsistencyChecker} from "jstests/libs/check_metadata_consistency_helpers.js";
import {DiscoverTopology, Topology} from "jstests/libs/discover_topology.js";

assert.neq(typeof db, 'undefined', 'No `db` object, is the shell connected to a server?');

const conn = db.getMongo();

const assertNonShardedCluster = (conn) => {
    try {
        const topology = DiscoverTopology.findConnectedNodes(conn);
        assert(topology.type != Topology.kShardedCluster &&
                   !(topology.type == Topology.kReplicaSet && topology.configsvr &&
                     TestData.testingReplicaSetEndpoint),
               "Metadata consistency check must be run against a sharded cluster");
    } catch (e) {
        jsTest.log(
            `Aborted metadata consistency check due to an error during topology discovery: ${e}`);
    }
};

try {
    MetadataConsistencyChecker.run(conn);
} catch (e) {
    if (e.code === ErrorCodes.CommandNotFound) {
        assertNonShardedCluster(conn);
    }
}
