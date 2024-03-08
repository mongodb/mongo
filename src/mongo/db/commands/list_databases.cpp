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

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/list_databases_common.h"
#include "mongo/db/commands/list_databases_gen.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/database_name.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/serialization_context.h"

namespace mongo {

// XXX: remove and put into storage api
std::intmax_t dbSize(const std::string& database);

namespace {

// Failpoint which causes to hang "listDatabases" cmd after acquiring global lock in IS mode.
MONGO_FAIL_POINT_DEFINE(hangBeforeListDatabases);

class CmdListDatabases final : public ListDatabasesCmdVersion1Gen<CmdListDatabases> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kOptIn;
    }
    bool adminOnly() const final {
        return true;
    }
    bool maintenanceOk() const final {
        return false;
    }
    std::string help() const final {
        return "{ listDatabases:1, [filter: <filterObject>] [, nameOnly: true ] }\n"
               "list databases on this server";
    }
    bool allowedWithSecurityToken() const final {
        return true;
    }

    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;

        bool supportsWriteConcern() const final {
            return false;
        }

        void doCheckAuthorization(OperationContext*) const final {}

        NamespaceString ns() const final {
            return NamespaceString(request().getDbName());
        }

        ListDatabasesReply typedRun(OperationContext* opCtx) final {
            CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);
            auto* as = AuthorizationSession::get(opCtx->getClient());
            auto cmd = request();

            // {nameOnly: bool} - default false.
            const bool nameOnly = cmd.getNameOnly();

            const auto& tenantId = cmd.getDbName().tenantId();

            // {authorizedDatabases: bool} - Dynamic default based on permissions.
            const bool authorizedDatabases = ([as, tenantId](const boost::optional<bool>& authDB) {
                const bool mayListAllDatabases = as->isAuthorizedForActionsOnResource(
                    ResourcePattern::forClusterResource(tenantId), ActionType::listDatabases);

                if (authDB) {
                    uassert(ErrorCodes::Unauthorized,
                            "Insufficient permissions to list all databases",
                            authDB.value() || mayListAllDatabases);
                    return authDB.value();
                }

                // By default, list all databases if we can, otherwise
                // only those we're allowed to find on.
                return !mayListAllDatabases;
            })(cmd.getAuthorizedDatabases());

            // {filter: matchExpression}.
            std::unique_ptr<MatchExpression> filter = list_databases::getFilter(cmd, opCtx, ns());

            std::vector<DatabaseName> dbNames;
            StorageEngine* storageEngine = getGlobalServiceContext()->getStorageEngine();
            {
                Lock::GlobalLock lk(opCtx, MODE_IS);
                CurOpFailpointHelpers::waitWhileFailPointEnabled(
                    &hangBeforeListDatabases, opCtx, "hangBeforeListDatabases", []() {});
                dbNames = storageEngine->listDatabases(tenantId);
            }
            std::vector<ListDatabasesReplyItem> items;
            SerializationContext scReply =
                SerializationContext::stateCommandReply(cmd.getSerializationContext());
            if (gMultitenancySupport && !tenantId) {
                // During the multitenancy upgrade process a mongod might receive listDatabases from
                // a non-multitenant node (with prefix and without token) during initial sync.
                scReply.setPrefixState(true);
            }
            int64_t totalSize = list_databases::setReplyItems(opCtx,
                                                              dbNames,
                                                              items,
                                                              storageEngine,
                                                              nameOnly,
                                                              filter,
                                                              false /* setTenantId */,
                                                              authorizedDatabases,
                                                              scReply);

            // We need to copy the serialization context from the request to the reply object
            ListDatabasesReply reply(std::move(items), scReply);
            if (!nameOnly) {
                reply.setTotalSize(totalSize);
                reply.setTotalSizeMb(totalSize / (1024 * 1024));
            }

            return reply;
        }
    };
};
MONGO_REGISTER_COMMAND(CmdListDatabases).forShard();
}  // namespace
}  // namespace mongo
