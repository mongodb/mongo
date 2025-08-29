/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"

#include <memory>
#include <string>
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
class ServiceLifecycle {
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
     * If true, this instance was initialized using the default syncdelay parameter rather than any
     * user-configured value.
     */
    virtual bool initializedUsingDefaultSyncDelay() const = 0;

    /**
     * If true, the named thread must be kept alive until the storage engine has shut down.
     */
    virtual bool shouldKeepThreadAliveUntilStorageEngineHasShutDown(
        StringData threadName) const = 0;
};

}  // namespace rss
}  // namespace mongo
