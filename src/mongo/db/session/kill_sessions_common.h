// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/session/kill_sessions.h"
#include "mongo/db/session/kill_sessions_gen.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/session_killer.h"
#include "mongo/rpc/metadata/audit_user_attrs.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/str.h"

#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <absl/container/node_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Local killing involves looping over all local operations, checking to see if they have matching
 * logical session ids, and killing if they do.
 */
SessionKiller::Result killSessionsLocalKillOps(OperationContext* opCtx,
                                               const SessionKiller::Matcher& matcher);

/**
 * Helper for executing a pattern set from a kill sessions style command.
 */
Status killSessionsCmdHelper(OperationContext* opCtx, const KillAllSessionsByPatternSet& patterns);

/**
 * Helper for logging kill sessions command.
 */
void killSessionsReport(OperationContext* opCtx, const BSONObj& cmdObj);

class ScopedKillAllSessionsByPatternImpersonator {
public:
    ScopedKillAllSessionsByPatternImpersonator(OperationContext* opCtx,
                                               const KillAllSessionsByPattern& pattern) {
        _opCtx = opCtx;
        // If pattern doesn't contain both user and roles, make sure we don't swap on destruction
        _active = pattern.getUsers() && pattern.getRoles();
        if (!_active) {
            return;
        }
        // Save current value of AuditUserAttrs for later restore
        _attrs = rpc::AuditUserAttrs::get(opCtx);
        // Set AuditUserAttrs to match the pattern passed in
        auto [name, roles] = getKillAllSessionsByPatternImpersonateData(pattern);
        if (name) {
            rpc::AuditUserAttrs::set(opCtx, *name, roles, true /* isImpersonating */);
        } else {
            rpc::AuditUserAttrs::resetToAuthenticatedUser(opCtx);
        }
    }

    ~ScopedKillAllSessionsByPatternImpersonator() {
        // Ensure the impersonator is active and opCtx is still alive, and swap back.
        if (_active && _opCtx->checkForInterruptNoAssert().isOK()) {
            if (_attrs) {
                rpc::AuditUserAttrs::set(_opCtx, *_attrs);
            } else {
                rpc::AuditUserAttrs::resetToAuthenticatedUser(_opCtx);
            }
        }
    }

private:
    OperationContext* _opCtx;
    boost::optional<rpc::AuditUserAttrs> _attrs;
    bool _active;
};

/**
 * This elaborate bit of artiface helps us to adapt the shape of a cursor manager that we know from
 * logical sessions with the different ways to cancel cursors in mongos versus mongod.  I.e. the
 * two types share no code, but do share enough shape to re-use some boilerplate.
 */
template <typename Eraser>
class [[MONGO_MOD_PUBLIC]] KillCursorsBySessionAdaptor {
public:
    KillCursorsBySessionAdaptor(OperationContext* opCtx,
                                const SessionKiller::Matcher& matcher,
                                Eraser&& eraser)
        : _opCtx(opCtx), _matcher(matcher), _cursorsKilled(0), _eraser(eraser) {}

    /**
     * Kills cursors in 'mgr' which belong to a session matching the SessionKilled::Matcher with
     * which this adaptor was constructed.
     */
    template <typename Mgr>
    void operator()(Mgr& mgr) noexcept {
        LogicalSessionIdSet activeSessions;
        mgr.appendActiveSessions(&activeSessions);

        for (const auto& session : activeSessions) {
            if (const KillAllSessionsByPattern* pattern = _matcher.match(session)) {
                ScopedKillAllSessionsByPatternImpersonator impersonator(_opCtx, *pattern);

                auto cursors = mgr.getCursorsForSession(session);
                for (const auto& id : cursors) {
                    try {
                        _eraser(mgr, id);
                        _cursorsKilled++;
                    } catch (const ExceptionFor<ErrorCodes::CursorNotFound>&) {
                        // Cursor was killed separately after this command was initiated. Still
                        // count the cursor as killed here, since the user's request is
                        // technically satisfied.
                        _cursorsKilled++;
                    } catch (const DBException& ex) {
                        _failures.push_back(ex.toStatus());
                    }
                }
            }
        }
    }

    /**
     * Returns an OK status if no errors were encountered during cursor killing, or a non-OK status
     * summarizing any errors encountered.
     */
    Status getStatus() const {
        if (_failures.empty()) {
            return Status::OK();
        }

        if (_failures.size() == 1) {
            return _failures.back();
        }

        return Status(_failures.back().code(),
                      str::stream() << "Encountered " << _failures.size()
                                    << " errors while killing cursors, "
                                       "showing most recent error: "
                                    << _failures.back().reason());
    }

    /**
     * Returns the number of cursors killed by operator().
     */
    int getCursorsKilled() const {
        return _cursorsKilled;
    }

private:
    OperationContext* _opCtx;
    const SessionKiller::Matcher& _matcher;
    std::vector<Status> _failures;
    int _cursorsKilled;
    Eraser _eraser;
};

template <typename Eraser>
[[MONGO_MOD_PUBLIC]] auto makeKillCursorsBySessionAdaptor(OperationContext* opCtx,
                                                          const SessionKiller::Matcher& matcher,
                                                          Eraser&& eraser) {
    return KillCursorsBySessionAdaptor<std::decay_t<Eraser>>{
        opCtx, matcher, std::forward<Eraser>(eraser)};
}

}  // namespace mongo
