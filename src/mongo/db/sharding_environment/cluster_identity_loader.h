/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

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
#include "mongo/stdx/mutex.h"
#include "mongo/util/hierarchical_acquisition.h"

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

    stdx::mutex _mutex;
    stdx::condition_variable _inReloadCV;

    // Used to ensure that only one thread at a time attempts to reload the cluster ID from the
    // config.version collection
    InitializationState _initializationState{InitializationState::kUninitialized};

    // Stores the result of the last call to _loadClusterId.  Used to cache the cluster ID once it
    // has been successfully loaded, as well as to report failures in loading across threads.
    StatusWith<OID> _lastLoadResult{Status{ErrorCodes::InternalError, "cluster ID never loaded"}};
};

}  // namespace mongo
