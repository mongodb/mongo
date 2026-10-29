/**
 * Test to ensure all internal appNames have been correctly accounted for with respect to the
 * ingress request rate limiter.
 *
 * Performs a symmetric comparison between ALL_KNOWN_APPNAMES (the source-verified set of appNames
 * the server produces for internal connections) and the authoritative exemption list
 * kInternalConnectionAppNameExemptions defined in the rate limiter helper. Every known internal
 * appName must be either matched by the exemption list or explicitly listed in KNOWN_RATE_LIMITED.
 *
 * @tags: [
 *   requires_no_mongod,
 * ]
 */

// kInternalConnectionAppNameExemptions is the authoritative exemption list. Its entries are treated
// as prefixes by matchesSet, so e.g. "NetworkInterfaceTL-Repl" matches ReplNetwork,
// ReplCoordExternNetwork, ReplNodeDbWorkerNetwork, and ReplicaSetMonitor-TaskExecutor;
// "NetworkInterfaceTL-Reshard" matches every resharding name.
import {kInternalConnectionAppNameExemptions} from "jstests/noPassthrough/admission/libs/ingress_request_rate_limiter_helper.js";

// ---------------------------------------------------------------------------
// Source-verified set of all internal MongoDB connection appNames.
//
// NetworkInterfaceTL appNames are "NetworkInterfaceTL-{name}", where {name} is the instance name
// passed to executor::makeNetworkInterface() (see makeInstanceName,
// src/mongo/executor/network_interface_factory.cpp). PrimaryOnlyService-derived interfaces use
// "{kServiceName}Network" (src/mongo/db/repl/primary_only_service.cpp).
//
// This list is derived by enumerating every non-test makeNetworkInterface() call site and
// PrimaryOnlyService subclass, plus the direct (non-NetworkInterfaceTL) connection appNames.
// TODO(SERVER-128236): Replace the hardcoded list with an automatically generated one.
// ---------------------------------------------------------------------------
const ALL_KNOWN_APPNAMES = new Set([
    // ---- Direct connection appNames (applicationName argument to a client connect) ----
    "Cloner", // src/mongo/db/cloner.cpp
    "InitialSyncCloner", // src/mongo/db/repl/initial_sync/all_database_cloner.cpp
    "FileCopyBasedInitialSyncer", // src/mongo/db/modules/enterprise/src/fcbis/file_copy_based_initial_syncer.cpp
    "OplogFetcher", // src/mongo/db/repl/oplog_fetcher.cpp
    "Rollback", // src/mongo/db/repl/bgsync.cpp
    "connection pool", // globalConnPool name; src/mongo/client/global_conn_pool.cpp
    // ---- Driver name in the hello handshake (matched by the exemption matcher) ----
    "MongoDB Internal Client", // src/mongo/client/dbclient_session.cpp
    // ---- NetworkInterfaceTL appNames ----
    "NetworkInterfaceTL-AddShard-TaskExecutor", // sharding_environment/sharding_initialization_mongod.cpp
    "NetworkInterfaceTL-AddShardCoordinator-TaskExecutor", // topology/add_shard_coordinator.cpp
    "NetworkInterfaceTL-ConfigsvrCoordinatorServiceNetwork", // POS: ConfigsvrCoordinatorService
    "NetworkInterfaceTL-DisaggNetwork", // modules/atlas/.../disaggregated_service_lifecycle.cpp
    "NetworkInterfaceTL-FLECrudNetwork", // db/fle_crud_mongod.cpp
    "NetworkInterfaceTL-FaultManager-TaskExecutor", // db/process_health/fault_manager.cpp
    "NetworkInterfaceTL-HelloMe-TaskExecutor", // topology/shardsvr_check_can_connect_to_config_server_cmd.cpp
    "NetworkInterfaceTL-MirrorMaestro", // db/mirror_maestro.cpp
    "NetworkInterfaceTL-MongotExecutor", // db/query/search/search_task_executors.cpp
    "NetworkInterfaceTL-MultiUpdateCoordinatorServiceNetwork", // POS: MultiUpdateCoordinatorService
    "NetworkInterfaceTL-OplogApplierNetwork", // repl/replication_coordinator_external_state_impl.cpp
    "NetworkInterfaceTL-OplogWriterNetwork", // repl/replication_coordinator_external_state_impl.cpp
    "NetworkInterfaceTL-QueryAnalysisWriterNetwork", // db/s/query_analysis_writer.cpp
    "NetworkInterfaceTL-RangeDeleterServiceExecutor", // db/s/range_deleter_service.cpp
    "NetworkInterfaceTL-RenameCollectionParticipantServiceNetwork", // POS: RenameCollectionParticipantService
    "NetworkInterfaceTL-ReplCoordExternNetwork", // repl/replication_coordinator_external_state_impl.cpp
    "NetworkInterfaceTL-ReplNetwork", // rss/attached_storage/attached_service_lifecycle.cpp
    "NetworkInterfaceTL-ReplNodeDbWorkerNetwork", // db/mongod_main.cpp
    "NetworkInterfaceTL-ReplicaSetMonitor-TaskExecutor", // client/replica_set_monitor_manager.cpp
    "NetworkInterfaceTL-ReshardingCollectionClonerNetwork", // db/s/resharding/resharding_data_replication.cpp
    "NetworkInterfaceTL-ReshardingCoordinatorServiceNetwork", // POS: ReshardingCoordinatorService
    "NetworkInterfaceTL-ReshardingDonorServiceNetwork", // POS: ReshardingDonorService
    "NetworkInterfaceTL-ReshardingOplogFetcherNetwork", // db/s/resharding/resharding_data_replication.cpp
    "NetworkInterfaceTL-ReshardingRecipientServiceNetwork", // POS: ReshardingRecipientService
    "NetworkInterfaceTL-SearchIndexMgmtExecutor", // db/query/search/search_task_executors.cpp
    "NetworkInterfaceTL-ServerDiscoveryMonitor-TaskExecutor", // client/server_discovery_monitor.cpp
    "NetworkInterfaceTL-ShardRegistryUpdater", // db/topology/shard_registry.cpp
    "NetworkInterfaceTL-Sharding-Fixed", // sharding_environment/sharding_initialization.cpp
    "NetworkInterfaceTL-ShardingCoordinatorNetwork", // POS: ShardingCoordinator
    "NetworkInterfaceTL-StandaloneNetwork", // db/local_executor.cpp (name from db/mongod_main.cpp)
    "NetworkInterfaceTL-TTLMonitorMetadataRefreshNetwork", // db/ttl/ttl_monitor.cpp
    // TaskExecutorPool creates one interface per pool member ("TaskExecutorPool-{i}");
    // "-0" is representative. sharding_environment/sharding_initialization.cpp
    "NetworkInterfaceTL-TaskExecutorPool-0",
]);

// ---------------------------------------------------------------------------
// Verified internal appNames that are NOT covered by the authoritative exemption list. Their
// connections are subject to the ingress request rate limiter. If any of these must instead be
// exempt, add it to kInternalConnectionAppNameExemptions in the helper (and the appNameExemptions
// anchor in buildscripts/resmokeconfig/matrix_suites/overrides/rate_limiter_with_auth.yml).
// ---------------------------------------------------------------------------
const KNOWN_RATE_LIMITED = new Set([
    "InitialSyncCloner",
    "FileCopyBasedInitialSyncer",
    "connection pool",
    "NetworkInterfaceTL-AddShard-TaskExecutor",
    "NetworkInterfaceTL-DisaggNetwork",
    "NetworkInterfaceTL-FLECrudNetwork",
    "NetworkInterfaceTL-FaultManager-TaskExecutor",
    "NetworkInterfaceTL-MirrorMaestro",
    "NetworkInterfaceTL-MongotExecutor",
    "NetworkInterfaceTL-MultiUpdateCoordinatorServiceNetwork",
    "NetworkInterfaceTL-OplogApplierNetwork",
    "NetworkInterfaceTL-OplogWriterNetwork",
    "NetworkInterfaceTL-QueryAnalysisWriterNetwork",
    "NetworkInterfaceTL-RangeDeleterServiceExecutor",
    "NetworkInterfaceTL-RenameCollectionParticipantServiceNetwork",
    "NetworkInterfaceTL-SearchIndexMgmtExecutor",
    "NetworkInterfaceTL-ServerDiscoveryMonitor-TaskExecutor",
    "NetworkInterfaceTL-ShardRegistryUpdater",
    "NetworkInterfaceTL-TTLMonitorMetadataRefreshNetwork",
    "NetworkInterfaceTL-TaskExecutorPool-0",
]);

// ---------------------------------------------------------------------------
// matchesSet returns true when any entry in `set` matches `name`. Entries are prefixes, so a short
// entry like "NetworkInterfaceTL-Repl" matches every appName that starts with it. Append "$" to an
// entry to require an exact match instead.
// ---------------------------------------------------------------------------

function matchesSet(name, set) {
    for (const entry of set) {
        if (entry.endsWith("$")) {
            if (name === entry.slice(0, -1)) return true;
        } else {
            if (name.startsWith(entry)) return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Core invariant: KNOWN_RATE_LIMITED must be exactly the set of known appNames the exemption list
// does NOT cover, i.e. KNOWN_RATE_LIMITED === {n in ALL_KNOWN_APPNAMES : !exempt(n)}.
//
// Deriving the expected rate-limited set from the exemption list (rather than filtering
// KNOWN_RATE_LIMITED out up front) means the check fails if an entry in KNOWN_RATE_LIMITED is in
// fact exempted — a contradiction we want surfaced, not silently accepted.
// ---------------------------------------------------------------------------
const expectedRateLimited = new Set(
    [...ALL_KNOWN_APPNAMES].filter((n) => !matchesSet(n, kInternalConnectionAppNameExemptions)),
);

// Known, non-exempt appNames missing from KNOWN_RATE_LIMITED: a new executor was added without
// accounting for it — exempt it, or declare it rate-limited.
const undeclaredRateLimited = [...expectedRateLimited]
    .filter((n) => !KNOWN_RATE_LIMITED.has(n))
    .sort();

// Entries in KNOWN_RATE_LIMITED that are actually exempt, or are not a known appName at all: the
// declaration is stale or contradicts the exemption list.
const misdeclaredRateLimited = [...KNOWN_RATE_LIMITED]
    .filter((n) => !expectedRateLimited.has(n))
    .sort();

// Exemption entries that match no known appName (renamed/removed component)
const unusedExemptions = [...kInternalConnectionAppNameExemptions]
    .filter((entry) => ![...ALL_KNOWN_APPNAMES].some((n) => matchesSet(n, new Set([entry]))))
    .sort();

assert.eq(
    undeclaredRateLimited.length,
    0,
    "Internal appNames in ALL_KNOWN_APPNAMES are neither exempt nor in KNOWN_RATE_LIMITED.\n" +
        "Add to kInternalConnectionAppNameExemptions in the helper (and the rate_limiter_with_auth.yml\n" +
        "anchor) if the connection must not be throttled, or to KNOWN_RATE_LIMITED otherwise.",
    {undeclaredRateLimited},
);

assert.eq(
    misdeclaredRateLimited.length,
    0,
    "AppNames in KNOWN_RATE_LIMITED are actually exempt (or are not known appNames).\n" +
        "Remove them from KNOWN_RATE_LIMITED, or fix the entry to match a real, non-exempt appName.",
    {misdeclaredRateLimited},
);

assert.eq(
    unusedExemptions.length,
    0,
    "Exempted appNames (kInternalConnectionAppNameExemptions) match no known internal appName.\n" +
        "The server component may have been renamed or removed; remove the stale entry from the\n" +
        "helper (and the rate_limiter_with_auth.yml anchor).",
    {unusedExemptions},
);

jsTest.log.info("SUCCESS: internal appNames exemption list is up to date.");
