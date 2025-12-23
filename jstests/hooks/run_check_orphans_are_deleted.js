/**
 * Asserts that no shard in the cluster contains any orphan documents.
 *
 * Note: This hook won't find documents which don't have the full shard key.
 */
import {CheckOrphansAreDeletedHelpers} from "jstests/libs/check_orphans_are_deleted_helpers.js";
import {DiscoverTopology, Topology} from "jstests/libs/discover_topology.js";
import newMongoWithRetry from "jstests/libs/retryable_mongo.js";

assert.neq(typeof db, 'undefined', 'No `db` object, is the shell connected to a server?');

const conn = db.getMongo();
const topology = DiscoverTopology.findConnectedNodes(conn);

if (topology.type == Topology.kShardedCluster) {
    for (let shardName of Object.keys(topology.shards)) {
        const shard = topology.shards[shardName];
        let shardPrimary;

        if (shard.type === Topology.kStandalone) {
            shardPrimary = shard.mongod;
        } else if (shard.type === Topology.kReplicaSet) {
            shardPrimary = shard.primary;
        } else {
            throw new Error('Unrecognized topology format: ' + tojson(topology));
        }

        CheckOrphansAreDeletedHelpers.runCheck(
            db.getMongo(), newMongoWithRetry(shardPrimary), shardName);
    }
} else if (topology.type == Topology.kReplicaSet && topology.configsvr &&
           TestData.testingReplicaSetEndpoint) {
    CheckOrphansAreDeletedHelpers.runCheck(
        db.getMongo(), newMongoWithRetry(topology.primary), "config");
} else {
    throw new Error('Orphan documents check must be run against a sharded cluster, but got: ' +
                    tojson(topology));
}
