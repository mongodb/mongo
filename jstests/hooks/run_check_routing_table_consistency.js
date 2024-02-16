import {
    RoutingTableConsistencyChecker
} from "jstests/libs/check_routing_table_consistency_helpers.js";
import {DiscoverTopology, Topology} from "jstests/libs/discover_topology.js";

assert.neq(typeof db, 'undefined', 'No `db` object, is the shell connected to a server?');

const conn = db.getMongo();
const topology = DiscoverTopology.findConnectedNodes(conn);

assert(topology.type == Topology.kShardedCluster ||
           (topology.type == Topology.kReplicaSet && topology.configsvr &&
            TestData.testingReplicaSetEndpoint),
       "Routing table consistency check must be run against a sharded cluster, but got: " +
           tojson(topology));

RoutingTableConsistencyChecker.run(db.getMongo());
