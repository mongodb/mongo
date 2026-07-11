// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/oid.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/modules.h"

#include <mutex>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>

namespace mongo {

class OperationContext;
class ServiceContext;
template <typename T>
class StatusWith;

/**
 * Decoration on ServiceContext used by any process in a sharded cluster to access the cluster ID.
 */
class ClusterIdentityLoader {
    ClusterIdentityLoader(const ClusterIdentityLoader&) = delete;
    ClusterIdentityLoader& operator=(const ClusterIdentityLoader&) = delete;

public:
    ClusterIdentityLoader() = default;
    ~ClusterIdentityLoader() = default;

    /**
     * Retrieves the ClusterIdentity object associated with the given service context.
     */
    static ClusterIdentityLoader* get(ServiceContext* serviceContext);
    static ClusterIdentityLoader* get(OperationContext* operationContext);

    /*
     * Returns the cached cluster ID.  Invalid to call unless loadClusterId has previously been
     * called and returned success.
     */
    OID getClusterId();

    /**
     * Loads the cluster ID from the config server's config.version collection and stores it into
     * _lastLoadResult.  If the cluster ID has previously been successfully loaded, this is a no-op.
     * If another thread is already in the process of loading the cluster ID, concurrent calls will
     * wait for that thread to finish and then return its results.
     */
    Status loadClusterId(OperationContext* opCtx,
                         ShardingCatalogClient* catalogClient,
                         const repl::ReadConcernLevel& readConcernLevel);

    /**
     * Called if the config.version document is rolled back.  Notifies the ClusterIdentityLoader
     * that the cached cluster ID is invalid and needs to be reloaded.
     */
    void discardCachedClusterId();

private:
    enum class InitializationState {
        kUninitialized,  // We have never successfully loaded the cluster ID
        kLoading,        // One thread is in the process of attempting to load the cluster ID
        kInitialized,    // We have been able to successfully load the cluster ID.
    };

    /**
     * Queries the config.version collection on the config server, extracts the cluster ID from
     * the version document, and returns it.
     */
    StatusWith<OID> _fetchClusterIdFromConfig(OperationContext* opCtx,
                                              ShardingCatalogClient* catalogClient,
                                              const repl::ReadConcernLevel& readConcernLevel);

    std::mutex _mutex;
    stdx::condition_variable _inReloadCV;

    // Used to ensure that only one thread at a time attempts to reload the cluster ID from the
    // config.version collection
    InitializationState _initializationState{InitializationState::kUninitialized};

    // Stores the result of the last call to _loadClusterId.  Used to cache the cluster ID once it
    // has been successfully loaded, as well as to report failures in loading across threads.
    StatusWith<OID> _lastLoadResult{Status{ErrorCodes::InternalError, "cluster ID never loaded"}};
};

}  // namespace mongo
