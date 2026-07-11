// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/session_killer.h"
#include "mongo/util/modules.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/time_support.h"

#include <functional>
#include <utility>

namespace mongo {

/**
 * A service-dependent type for the LogicalSessionCache to use to find the
 * current time, schedule periodic refresh jobs, and get a list of sessions
 * that are being used for long-running queries on the service context.
 *
 * Mongod and mongos implement their own classes to fulfill this interface.
 */
class ServiceLiaison {
public:
    virtual ~ServiceLiaison();

    /**
     * Return a list of sessions that are currently being used to run operations
     * on this service.
     */
    virtual LogicalSessionIdSet getActiveOpSessions() const = 0;

    /**
     * Return a list of sessions that are currently attached to open cursors
     */
    virtual LogicalSessionIdSet getOpenCursorSessions(OperationContext* opCtx) const = 0;

    /**
     * Schedule a job to be run at regular intervals until the server shuts down.
     *
     * The ServiceLiaison should start its background runner on construction, and
     * should continue fielding job requests through scheduleJob until join() is
     * called.
     */
    virtual void scheduleJob(PeriodicRunner::PeriodicJob job) = 0;

    /**
     * Stops the service liaison from running any more jobs scheduled
     * through scheduleJob. This method may block and wait for background threads to
     * join. Implementations should make it safe for this method to be called
     * multiple times, or concurrently by different threads.
     */
    virtual void join() = 0;

    /**
     * Return the current time.
     */
    virtual Date_t now() const = 0;

    /**
     * Delegates to a similarly named function on a cursor manager.
     */
    virtual int killCursorsWithMatchingSessions(OperationContext* opCtx,
                                                const SessionKiller::Matcher& matcher) = 0;

protected:
    /**
     * Returns the service context.
     */
    virtual ServiceContext* _context() = 0;
};

}  // namespace mongo
