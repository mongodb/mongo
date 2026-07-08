import {RoutingTableConsistencyChecker} from "jstests/libs/check_routing_table_consistency_helpers.js";
import {DiscoverTopology, Topology} from "jstests/libs/discover_topology.js";

assert.neq(typeof db, "undefined", "No `db` object, is the shell connected to a server?");

const conn = db.getMongo();
const topology = DiscoverTopology.findConnectedNodes(conn);

assert(
    topology.type == Topology.kShardedCluster ||
        (topology.type == Topology.kReplicaSet && topology.configsvr),
    "Routing table consistency check must be run against a sharded cluster, but got: " +
        tojson(topology),
);

// TODO SERVER-130827 The following assert() call verifies that the current topology is not a
// replica set topology. It is already checked by the assert above that the topology is either a
// sharded cluster or a replica set topology.
// The assert below is only here to establish if this hook is currently called for replica set
// test fixtures or not. If the following assert triggers, it means the hook is still used by
// replica set fixtures, and then the following assert must be removed again. If the following
// assert is not triggered, then it can be removed, and the assert above can be simplified so
// that it fails on any other topology than sharded clusters.
assert(
    !(topology.type == Topology.kReplicaSet && topology.configsvr),
    "Replica set topology is not expected here",
);

RoutingTableConsistencyChecker.run(db.getMongo());
