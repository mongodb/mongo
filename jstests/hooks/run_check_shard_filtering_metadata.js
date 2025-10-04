/**
 * Checks the filtering metadata on the shards matches the one in the configsvr.
 */
import {CheckShardFilteringMetadataHelpers} from "jstests/libs/check_shard_filtering_metadata_helpers.js";
import {DiscoverTopology, Topology} from "jstests/libs/discover_topology.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import newMongoWithRetry from "jstests/libs/retryable_mongo.js";

assert.neq(typeof db, "undefined", "No `db` object, is the shell connected to a server?");

const conn = db.getMongo();

let topology;
try {
    topology = DiscoverTopology.findConnectedNodes(conn);
} catch (e) {
    if (CheckShardFilteringMetadataHelpers.isTransientError(e)) {
        jsTest.log(`Aborted filtering metadata check due to retriable error during topology discovery: ${e}`);
        quit();
    } else {
        throw e;
    }
}

if (topology.type !== Topology.kShardedCluster) {
    throw new Error("Filtering metadata check can only be run against a sharded cluster, but got: " + tojson(topology));
}

try {
    for (let shardName of Object.keys(topology.shards)) {
        const shard = topology.shards[shardName];

        if (shard.type !== Topology.kReplicaSet) {
            throw new Error("Unexpected topology: " + tojson(topology));
        }

        // Await replication to ensure that metadata on secondary nodes is up-to-date.
        new ReplSetTest(shard.nodes[0]).awaitSecondaryNodes();

        // Skipping checking sharded collection metadata because any workload on the suite could
        // perform an operation that is known to leave incorrect metadata (such as
        // refineCollectionShardKey).
        shard.nodes.forEach((node) => {
            CheckShardFilteringMetadataHelpers.run(
                db.getMongo(),
                newMongoWithRetry(node),
                shardName,
                true /* skipCheckShardedCollections */,
            );
        });
    }
} catch (e) {
    if (CheckShardFilteringMetadataHelpers.isTransientError(e)) {
        jsTest.log(`Aborted filtering metadata check due to retriable error: ${e}`);
        quit();
    } else {
        throw e;
    }
}
