// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/session/service_liaison_impl.h"

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"

#include <mutex>

#include <absl/container/node_hash_map.h>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {

LogicalSessionIdSet ServiceLiaisonImpl::getActiveOpSessions() const {
    LogicalSessionIdSet activeSessions;

    invariant(hasGlobalServiceContext());

    // Walk through the service context and append lsids for all currently-running ops.
    for (ServiceContext::LockedClientsCursor cursor(getGlobalServiceContext());
         Client* client = cursor.next();) {

        std::lock_guard<Client> lk(*client);
        auto clientOpCtx = client->getOperationContext();

        // Ignore clients without currently-running operations
        if (!clientOpCtx)
            continue;

        // Append this op ctx's session to our list, if it has one
        auto lsid = clientOpCtx->getLogicalSessionId();
        if (lsid) {
            activeSessions.insert(*lsid);
        }
    }
    return activeSessions;
}

LogicalSessionIdSet ServiceLiaisonImpl::getOpenCursorSessions(OperationContext* opCtx) const {
    return _getOpenCursorsFn(opCtx);
}

void ServiceLiaisonImpl::scheduleJob(PeriodicRunner::PeriodicJob job) {
    invariant(hasGlobalServiceContext());
    auto jobAnchor = getGlobalServiceContext()->getPeriodicRunner()->makeJob(std::move(job));
    jobAnchor.start();

    {
        std::lock_guard lk(_mutex);
        _jobs.push_back(std::move(jobAnchor));
    }
}

void ServiceLiaisonImpl::join() {
    auto jobs = [&] {
        std::lock_guard lk(_mutex);
        return std::exchange(_jobs, {});
    }();
}

Date_t ServiceLiaisonImpl::now() const {
    invariant(hasGlobalServiceContext());
    return getGlobalServiceContext()->getFastClockSource()->now();
}

ServiceContext* ServiceLiaisonImpl::_context() {
    return getGlobalServiceContext();
}

int ServiceLiaisonImpl::killCursorsWithMatchingSessions(OperationContext* opCtx,
                                                        const SessionKiller::Matcher& matcher) {
    return _killCursorsFn(opCtx, matcher);
}

}  // namespace mongo
