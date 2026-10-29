// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

    bool requiresAuthzChecks() const override {
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
