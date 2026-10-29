// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/service_context.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/periodic_runner.h"

#include <memory>
#include <variant>

namespace mongo {

class AuthorizationManager;
class OperationContext;

/**
 * Background job that runs only in mongos and periodically checks in with the config servers
 * to determine whether any authorization information has changed, and if so causes the
 * AuthorizationManager to throw out its in-memory cache of User objects (which contains the
 * users' credentials, roles, privileges, etc).
 */
class [[MONGO_MOD_PUBLIC]] UserCacheInvalidator {
public:
    using OIDorTimestamp = std::variant<OID, Timestamp>;

    /**
     * Create a new UserCacheInvalidator as a decorator on the service context
     * and start the background job.
     */
    static void start(ServiceContext* serviceCtx, OperationContext* opCtx);

    /**
     * Waits for the job to complete and stops the thread.
     */
    static void stop(ServiceContext* serviceCtx);

    /**
     * Set the period of the background job. This should only be used internally (by the
     * setParameter).
     */
    void setPeriod(Milliseconds period);

private:
    void initialize(OperationContext* opCtx);
    void run();

    PeriodicJobAnchor _job;

    OIDorTimestamp _previousGeneration;

    // this mutex serializes start and stop+detach on the periodic job
    std::mutex _jobMutex;
};

Status userCacheInvalidationIntervalSecsNotify(const int& newValue);

}  // namespace mongo
