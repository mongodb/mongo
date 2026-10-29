// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/rss/service_lifecycle.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo::rss {

class AttachedServiceLifecycle : public ServiceLifecycle {
public:
    AttachedServiceLifecycle() = default;

    /**
     * Initializes flow control based on oplog write rate.
     */
    void initializeFlowControl(ServiceContext*) override;

    /**
     * There are no storage engine extensions utilized.
     */
    void initializeStorageEngineExtensions(ServiceContext*) override;

    /**
     * Initializes a 'repl::ReplicationCoordinatorImpl'.
     */
    std::unique_ptr<repl::ReplicationCoordinator> initializeReplicationCoordinator(
        ServiceContext*) override;

    /**
     * There is no additional state required for storage access.
     */
    void initializeStateRequiredForStorageAccess(ServiceContext*) override;

    /**
     * There is no additional state required for storage access.
     */
    void shutdownStateRequiredForStorageAccess(ServiceContext*, BSONObjBuilder*) override;

    /**
     * There is no additional state required for offline validation.
     */
    void initializeStateRequiredForOfflineValidation(OperationContext*) override;

    /**
     * There are no specific persistence threads that must outlive the storage engine.
     */
    bool shouldKeepThreadAliveUntilStorageEngineHasShutDown(std::string_view) const override;
};

}  // namespace mongo::rss
