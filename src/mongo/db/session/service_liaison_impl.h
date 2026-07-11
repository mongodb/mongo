// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/service_liaison.h"
#include "mongo/db/session/session_killer.h"
#include "mongo/util/modules.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/time_support.h"

#include <mutex>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>

namespace mongo {

/**
 * This is the service liaison for the logical session cache.
 *
 * This class will return active sessions for cursors stored in the
 * global cursor manager and cursors in per-collection managers. This
 * class will also walk the service context to find all sessions for
 * currently-running operations on this server.
 *
 * Job scheduling on this class will be handled behind the scenes by a
 * periodic runner for this mongos. The time will be returned from the
 * system clock.
 */
class [[MONGO_MOD_PUBLIC]] ServiceLiaisonImpl : public ServiceLiaison {
public:
    using GetOpenCursorsFn = unique_function<LogicalSessionIdSet(OperationContext*)>;

    using KillCursorsFn = unique_function<int(OperationContext*, const SessionKiller::Matcher&)>;

    ServiceLiaisonImpl(GetOpenCursorsFn getOpenCursorsFn, KillCursorsFn killCursorsFn)
        : _getOpenCursorsFn(std::move(getOpenCursorsFn)),
          _killCursorsFn(std::move(killCursorsFn)) {};

    LogicalSessionIdSet getActiveOpSessions() const override;
    LogicalSessionIdSet getOpenCursorSessions(OperationContext* opCtx) const override;

    void scheduleJob(PeriodicRunner::PeriodicJob job) override;

    void join() override;

    Date_t now() const override;

    int killCursorsWithMatchingSessions(OperationContext* opCtx,
                                        const SessionKiller::Matcher& matcher) override;

protected:
    /**
     * Returns the service context.
     */
    ServiceContext* _context() override;

    std::mutex _mutex;
    std::vector<PeriodicJobAnchor> _jobs;

private:
    const GetOpenCursorsFn _getOpenCursorsFn;
    const KillCursorsFn _killCursorsFn;
};

}  // namespace mongo
