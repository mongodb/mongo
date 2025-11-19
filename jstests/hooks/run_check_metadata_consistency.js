import {MetadataConsistencyChecker} from "jstests/libs/check_metadata_consistency_helpers.js";
import {DiscoverTopology, Topology} from "jstests/libs/discover_topology.js";

assert.neq(typeof db, 'undefined', 'No `db` object, is the shell connected to a server?');

const conn = db.getMongo();

const assertNonShardedCluster = (conn) => {
    let topology;
    try {
        topology = DiscoverTopology.findConnectedNodes(conn);
    } catch (e) {
        jsTest.log.info(
            `Aborted metadata consistency check due to an error during topology discovery: ${e}`);
        return;
    }

    assert(
        topology && topology.type != Topology.kShardedCluster &&
            !(topology.type == Topology.kReplicaSet && topology.configsvr &&
              TestData.testingReplicaSetEndpoint),
        "Metadata consistency check command not found, but we are unexpectedly on a sharded cluster",
    );
};

try {
    MetadataConsistencyChecker.run(conn);
} catch (e) {
    if (e.code === ErrorCodes.CommandNotFound) {
        assertNonShardedCluster(conn);
    }
}
