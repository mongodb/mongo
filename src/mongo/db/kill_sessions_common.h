/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include "mongo/db/kill_sessions.h"

#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/session_killer.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/stringutils.h"

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
Status killSessionsCmdHelper(OperationContext* opCtx,
                             BSONObjBuilder& result,
                             const KillAllSessionsByPatternSet& patterns);

class ScopedKillAllSessionsByPatternImpersonator {
public:
    ScopedKillAllSessionsByPatternImpersonator(OperationContext* opCtx,
                                               const KillAllSessionsByPattern& pattern) {
        AuthorizationSession* authSession = AuthorizationSession::get(opCtx->getClient());

        if (pattern.getUsers() && pattern.getRoles()) {
            std::tie(_names, _roles) = getKillAllSessionsByPatternImpersonateData(pattern);
            _raii.emplace(authSession, &_names, &_roles);
        }
    }

private:
    std::vector<UserName> _names;
    std::vector<RoleName> _roles;
    boost::optional<AuthorizationSession::ScopedImpersonate> _raii;
};

/**
 * This elaborate bit of artiface helps us to adapt the shape of a cursor manager that we know from
 * logical sessions with the different ways to cancel cursors in mongos versus mongod.  I.e. the
 * two types share no code, but do share enough shape to re-use some boilerplate.
 */
template <typename Eraser>
class KillSessionsCursorManagerVisitor {
public:
    KillSessionsCursorManagerVisitor(OperationContext* opCtx,
                                     const SessionKiller::Matcher& matcher,
                                     Eraser&& eraser)
        : _opCtx(opCtx), _matcher(matcher), _cursorsKilled(0), _eraser(eraser) {}

    template <typename Mgr>
    void operator()(Mgr& mgr) {
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
                    } catch (...) {
                        _failures.push_back(exceptionToStatus());
                    }
                }
            }
        }
    }

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
auto makeKillSessionsCursorManagerVisitor(OperationContext* opCtx,
                                          const SessionKiller::Matcher& matcher,
                                          Eraser&& eraser) {
    return KillSessionsCursorManagerVisitor<std::decay_t<Eraser>>{
        opCtx, matcher, std::forward<Eraser>(eraser)};
}

}  // namespace mongo
