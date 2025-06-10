# Represents a DAG describing the complexity of suites. As we go deeper into the graph,
# suites get simpler. For example, suppose we have suite_A and suite_A_B, where suite_A_B
# tests a strict superset of *features* (not tests) tested by suite_A. Then, the graph
# would contain:
# {
#    suite_A_B: {
#       suite_A: {}
#    }
# }
#
# But suppose feature_B changed the interaction with the server fundamentally - for example,
# if feature_B was retryable writes - then in this case suite_A_B would not be a strict superset
# of the features tested by suite_A. In this case, suite_A_B cannot be considered more complex
# than suite_A; the two are incomparable.
SUITE_HIERARCHY = {
    # Concurrency suites
    "concurrency": {},
    "concurrency_compute_mode": {},
    "concurrency_multitenancy_replication_with_atlas_proxy": {},
    "simulate_crash_concurrency_replication": {},
    "concurrency_sharded_replication_with_balancer_and_config_transitions_and_add_remove_shard": {
        "concurrency_sharded_with_balancer_and_auto_bootstrap": {},
        "concurrency_sharded_replication_with_balancer": {},
    },
    "concurrency_sharded_replication_with_balancer_and_config_transitions": {
        # The auto_bootstrap suites maintain a static config shard and so the config_transitions
        # suite is a superset of it because it also transitions the config shard to a dedicated
        # replica set.
        "concurrency_sharded_with_balancer_and_auto_bootstrap": {
            "concurrency_sharded_with_auto_bootstrap": {}
        },
        "concurrency_sharded_replication_with_balancer": {"concurrency_sharded_replication": {}},
    },
    "concurrency_sharded_causal_consistency_and_balancer": {
        "concurrency_sharded_causal_consistency": {}
    },
    "concurrency_sharded_local_read_write_multi_stmt_txn_with_balancer": {
        "concurrency_sharded_local_read_write_multi_stmt_txn": {}
    },
    "concurrency_sharded_multi_stmt_txn_with_balancer_and_config_transitions_and_add_remove_shard": {
        "concurrency_sharded_multi_stmt_txn_with_balancer": {
            "concurrency_sharded_multi_stmt_txn": {}
        }
    },
    "concurrency_sharded_multi_stmt_txn_stepdown_terminate_kill_primary": {
        "concurrency_sharded_multi_stmt_txn": {}
    },
    "concurrency_sharded_stepdown_terminate_kill_primary_with_balancer_and_config_transitions_and_add_remove_shard": {
        "concurrency_sharded_stepdown_terminate_kill_primary_with_balancer": {
            # The stepdown suite is not considered a superset of concurrency_sharded_replication
            # because the stepdown suite uses retryable writes whereas the vanilla suite does not.
            # Therefore the commands being sent to the server are fundamentally different.
            "concurrency_sharded_with_stepdowns": {}
        }
    },
    "concurrency_sharded_clusterwide_ops_add_remove_shards": {},
    "concurrency_replication_causal_consistency": {},
    "concurrency_replication_causal_consistency_with_replica_set_endpoint": {},
    "concurrency_replication_for_backup_restore": {},
    "concurrency_replication_for_export_import": {},
    "concurrency_replication_multi_stmt_txn": {},
    "concurrency_replication_multi_stmt_txn_with_replica_set_endpoint": {},
    "concurrency_replication_with_replica_set_endpoint": {},
    "concurrency_replication": {},
    "concurrency_sharded_initial_sync": {"concurrency_sharded_causal_consistency": {}},
    # JScore passthrough suites
    "replica_sets_reconfig_kill_stepdown_terminate_jscore_passthrough": {
        # The reconfig stepdown suite is not considered a superset of replica_sets_reconfig_jscore_passthrough suite because the stepdown suite uses retryable writes whereas the vanilla suite does not. Therefore the commands being sent to the server are fundamentally different.
        "replica_sets_reconfig_jscore_stepdown_passthrough": {},
        "replica_sets_reconfig_kill_primary_jscore_passthrough": {},
    },
    "replica_sets_multi_stmt_txn_kill_stepdown_terminate_jscore_passthrough": {
        "replica_sets_multi_stmt_txn_terminate_primary_jscore_passthrough": {},
        "replica_sets_multi_stmt_txn_stepdown_jscore_passthrough": {},
        "replica_sets_multi_stmt_txn_kill_primary_jscore_passthrough": {},
        "replica_sets_multi_stmt_txn_jscore_passthrough": {},
    },
    "replica_sets_initsync_logical_fcbis_jscore_passthrough": {
        "replica_sets_fcbis_jscore_passthrough": {},
        "replica_sets_initsync_jscore_passthrough": {},
    },
}


def compute_dag(complexity_graph):
    """
    Computes a graph of the below form from the nested complexity graph.
    {
        node: { parents: set(...), children: set(...) },
        ...
    }
    where the parents are the direct parents and children are direct
    children. Note that if the original nested graph had multiple paths connecting
    a node to another (A->B->C and A->C are both edges), then C would have both
    B and A as its direct parents, and A would have B and C as its direct children
    """
    graph = {}

    # Initially place all the known ancestor nodes in the frontier.
    frontier = []
    for ancestor, descendants in complexity_graph.items():
        frontier.append((ancestor, descendants))

    while frontier:
        parent, children = frontier.pop(0)
        if parent not in graph:
            graph[parent] = {"parents": set(), "children": set()}

        for child, grandchildren in children.items():
            if child not in graph:
                graph[child] = {"parents": set(), "children": set()}
            graph[parent]["children"].add(child)
            graph[child]["parents"].add(parent)

            frontier.append((child, grandchildren))

    return graph


def compute_ancestors(node, dag):
    """Returns all the ancestor, direct and indirect, of the given node."""
    ancestors = set()

    frontier = [node]
    while frontier:
        node = frontier.pop(0)
        ancestors = ancestors.union(dag[node]["parents"])
        frontier += list(dag[node]["parents"])

    return ancestors


def compute_minimal_test_set(suite_name, dag, tests_in_suite):
    """
    Given a DAG that represents which suite is more complex than
    another suite, and the set of tests that are usually run in each suite,
    returns the minimal set of tests that need to be run for the given suite.
    suite_name: the suite for which we want to calculate the minimal test set.
    dag: a dictionary of dictionaries of the form:
         {
            "node_N" : {
                "parents": set(["node_i", ...]),
                "children": set(["node_k", ...])
            }
        }
    tests_in_suite: a dictionary of suite_name -> set(tests) that can be run in the suite.
    """

    # To calculate the minimal set for a suite 'curr_suite':
    # 1) Figure out who all the ancestors of 'curr_suite' are.
    # 2) Subtract from curr_suite's test set the union of the test sets of all its ancestors.

    ancestors = compute_ancestors(suite_name, dag)
    # Copy the given set
    curr_test_set = set(tests_in_suite[suite_name])

    for ancestor in ancestors:
        curr_test_set -= tests_in_suite[ancestor]

    return curr_test_set
