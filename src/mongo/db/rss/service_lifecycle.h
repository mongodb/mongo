// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace mongo {
namespace rss {

/**
 * This class provides an abstraction for a set of functionalities related to the service lifecycle
 * (i.e. startup and shutdown).
 *
 * The implementation details are generally closely related to the configured 'PersistenceProvider',
 * but we separate it out from that class since that class is primarily focused on
 * capabilities/behaviors, while this class instead represents a set of setup/teardown and related
 * routines.
 */
class [[MONGO_MOD_OPEN]] ServiceLifecycle {
public:
    virtual ~ServiceLifecycle() = default;

    /**
     * Initializes the flow control algorithm for the current service configuration.
     */
    virtual void initializeFlowControl(ServiceContext*) = 0;

    /**
     * Initializes any storage engine extensions necessary for the current service configuration.
     */
    virtual void initializeStorageEngineExtensions(ServiceContext*) = 0;

    /**
     * Initializes and returns the replication coordinator appropriate for the current service
     * configuration.
     */
    virtual std::unique_ptr<repl::ReplicationCoordinator> initializeReplicationCoordinator(
        ServiceContext*) = 0;

    /**
     * Initializes any state required to access 'repl::StorageInterface'. This method will be run
     * prior to 'initializeReplicationCoordinator'.
     */
    virtual void initializeStateRequiredForStorageAccess(ServiceContext*) = 0;

    /**
     * Tears down any state set up by 'initializeStateRequiredForStorageAccess'.
     */
    virtual void shutdownStateRequiredForStorageAccess(ServiceContext*, BSONObjBuilder*) = 0;

    /**
     * Initializes any state required to run offline validation, also referred to as modal
     * validation. This is a special mode where the server validates its collections upon
     * startup and proceeds to shut down once the validation is complete. The validation
     * results are written to the logs. No user connections are accepted in this mode.
     *
     * This method is not called when the node is part of a replica set or handling user
     * requests as a standalone.
     */
    virtual void initializeStateRequiredForOfflineValidation(OperationContext*) = 0;

    /**
     * If true, the named thread must be kept alive until the storage engine has shut down.
     */
    virtual bool shouldKeepThreadAliveUntilStorageEngineHasShutDown(
        std::string_view threadName) const = 0;
};

}  // namespace rss
}  // namespace mongo
