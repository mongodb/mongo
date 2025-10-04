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

#include "mongo/db/auth/auth_name.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/parsed_privilege_gen.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/connection_status_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/read_through_cache.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class CmdConnectionStatus : public TypedCommand<CmdConnectionStatus> {
public:
    using Request = ConnectionStatusCommand;
    using Reply = typename ConnectionStatusCommand::Reply;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Reply typedRun(OperationContext* opCtx) {
            auto* as = AuthorizationSession::get(opCtx->getClient());

            ConnectionStatusReplyAuthInfo info;
            std::vector<UserName> userNames;
            if (auto userName = as->getAuthenticatedUserName()) {
                userNames.push_back(std::move(userName.value()));
            }
            info.setAuthenticatedUsers(std::move(userNames));
            info.setAuthenticatedUserRoles(
                iteratorToVector<RoleName>(as->getAuthenticatedRoleNames()));
            if (request().getShowPrivileges()) {
                info.setAuthenticatedUserPrivileges(expandPrivileges(as));
            }

            Reply reply;
            reply.setAuthInfo(std::move(info));
            reply.setUuid(opCtx->getClient()->getUUID());
            return reply;
        }

    private:
        template <typename T>
        static std::vector<T> iteratorToVector(AuthNameIterator<T> it) {
            std::vector<T> ret;
            for (; it.more(); it.next()) {
                ret.push_back(*it);
            }
            return ret;
        }

        static std::vector<auth::ParsedPrivilege> expandPrivileges(AuthorizationSession* as) {
            // Create a unified map of resources to privileges, to avoid duplicate
            // entries in the connection status output.
            User::ResourcePrivilegeMap unified;

            if (auto authUser = as->getAuthenticatedUser()) {
                for (const auto& privIter : authUser.value()->getPrivileges()) {
                    auto it = unified.find(privIter.first);
                    if (it == unified.end()) {
                        unified[privIter.first] = privIter.second;
                    } else {
                        it->second.addActions(privIter.second.getActions());
                    }
                }
            }

            std::vector<auth::ParsedPrivilege> ret;
            std::transform(unified.cbegin(),
                           unified.cend(),
                           std::back_inserter(ret),
                           [](const auto& it) { return it.second.toParsedPrivilege(); });
            return ret;
        }

        bool supportsWriteConcern() const final {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            // No auth required
        }

        NamespaceString ns() const final {
            return NamespaceString(request().getDbName());
        }
    };

    bool requiresAuth() const final {
        return false;
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }
};
MONGO_REGISTER_COMMAND(CmdConnectionStatus).forRouter().forShard();

}  // namespace mongo
