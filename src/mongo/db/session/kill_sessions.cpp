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

#include "mongo/db/session/kill_sessions.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/auth_name.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/read_through_cache.h"

#include <memory>
#include <set>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

namespace {

std::vector<KillAllSessionsUser> getKillAllSessionsImpersonateUsers(OperationContext* opCtx) {
    auto* as = AuthorizationSession::get(opCtx->getClient());

    std::vector<KillAllSessionsUser> out;

    if (auto name = as->getAuthenticatedUserName()) {
        out.emplace_back();
        out.back().setUser(name->getUser());
        out.back().setDb(name->getDB());
    }

    return out;
}

std::vector<KillAllSessionsRole> getKillAllSessionsImpersonateRoles(OperationContext* opCtx) {
    AuthorizationSession* authSession = AuthorizationSession::get(opCtx->getClient());

    std::vector<KillAllSessionsRole> out;

    for (auto iter = authSession->getAuthenticatedRoleNames(); iter.more(); iter.next()) {
        out.emplace_back();
        out.back().setRole(iter->getRole());
        out.back().setDb(iter->getDB());
    }

    return out;
}

}  // namespace

std::tuple<boost::optional<UserName>, std::vector<RoleName>>
getKillAllSessionsByPatternImpersonateData(const KillAllSessionsByPattern& pattern) {
    std::tuple<boost::optional<UserName>, std::vector<RoleName>> out;

    auto& user = std::get<0>(out);
    auto& roles = std::get<1>(out);

    if (pattern.getUsers() && (pattern.getUsers()->size() > 0)) {
        uassert(ErrorCodes::BadValue,
                "Too many users in impersonation data",
                pattern.getUsers()->size() <= 1);
        const auto& impUser = pattern.getUsers().value()[0];
        user = UserName(impUser.getUser(), impUser.getDb());
    }

    if (pattern.getRoles()) {
        roles.reserve(pattern.getRoles()->size());

        for (auto&& role : pattern.getRoles().value()) {
            roles.emplace_back(role.getRole(), role.getDb());
        }
    }

    return out;
}

KillAllSessionsByPatternItem makeKillAllSessionsByPattern(OperationContext* opCtx) {
    KillAllSessionsByPattern kasbp;

    kasbp.setUsers(getKillAllSessionsImpersonateUsers(opCtx));
    kasbp.setRoles(getKillAllSessionsImpersonateRoles(opCtx));
    return {kasbp, APIParameters::get(opCtx)};
}

KillAllSessionsByPatternItem makeKillAllSessionsByPattern(OperationContext* opCtx,
                                                          const KillAllSessionsUser& kasu) {
    KillAllSessionsByPatternItem item = makeKillAllSessionsByPattern(opCtx);

    User user(
        std::make_unique<UserRequestGeneral>(UserName(kasu.getUser(), kasu.getDb()), boost::none));
    item.pattern.setUid(user.getDigest());
    return item;
}

KillAllSessionsByPatternSet makeSessionFilterForAuthenticatedUsers(OperationContext* opCtx) {
    auto* as = AuthorizationSession::get(opCtx->getClient());
    KillAllSessionsByPatternSet patterns;

    if (auto user = as->getAuthenticatedUser()) {
        KillAllSessionsByPattern pattern;
        pattern.setUid(user.value()->getDigest());
        KillAllSessionsByPatternItem item{std::move(pattern), APIParameters::get(opCtx)};
        patterns.emplace(std::move(item));
    }
    return patterns;
}

KillAllSessionsByPatternItem makeKillAllSessionsByPattern(OperationContext* opCtx,
                                                          const LogicalSessionId& lsid) {
    auto item = makeKillAllSessionsByPattern(opCtx);
    item.pattern.setLsid(lsid);
    return item;
}

}  // namespace mongo
