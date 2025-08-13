/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/local_catalog/ddl/list_databases_common.h"
#include "mongo/db/local_catalog/ddl/list_databases_for_all_tenants_gen.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/explain_verbosity_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>

namespace mongo {

namespace {

class CmdListDatabasesForAllTenants final : public TypedCommand<CmdListDatabasesForAllTenants> {
public:
    using Request = ListDatabasesForAllTenantsCommand;
    using Reply = ListDatabasesForAllTenantsReply;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kOptIn;
    }

    bool adminOnly() const override {
        return true;
    }

    bool maintenanceOk() const override {
        return false;
    }

    std::string help() const override {
        return "{ listDatabasesForAllTenants:1, [filter: <filterObject>] [, nameOnly: true ] }\n"
               "command which lists databases for all tenants on this server";
    }

    bool skipApiVersionCheck() const override {
        return true;
    }

    const AuthorizationContract* getAuthorizationContract() const final {
        return &Request::kAuthorizationContract;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        bool supportsWriteConcern() const override {
            return false;
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            AuthorizationSession* authzSession = AuthorizationSession::get(opCtx->getClient());
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    authzSession->isAuthorizedForActionsOnResource(
                        ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                        ActionType::internal));
        }

        Reply typedRun(OperationContext* opCtx) {
            CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);
            uassert(ErrorCodes::CommandNotSupported,
                    "Multitenancy not enabled, command not supported",
                    gMultitenancySupport);

            auto cmd = request();

            // {nameOnly: bool} - default false.
            const bool nameOnly = cmd.getNameOnly();

            // {filter: matchExpression}.
            std::unique_ptr<MatchExpression> filter = list_databases::getFilter(cmd, opCtx, ns());

            std::vector<DatabaseName> dbNames;
            {
                // Read lock free through a consistent in-memory catalog and storage snapshot.
                AutoReadLockFree lockFreeReadBlock(opCtx);
                auto catalog = CollectionCatalog::get(opCtx);
                dbNames = catalog->getAllConsistentDbNames(opCtx);
            }

            std::vector<ListDatabasesForAllTenantsReplyItem> items;
            // Always return the dbName without the tenantId prefix as we set tenant id in
            // "tenantId" of the reply items.
            SerializationContext scReply = SerializationContext::stateCommandReply();
            scReply.setPrefixState(false);
            int64_t totalSize =
                list_databases::setReplyItems(opCtx,
                                              dbNames,
                                              items,
                                              getGlobalServiceContext()->getStorageEngine(),
                                              nameOnly,
                                              filter,
                                              true /* setTenantId */,
                                              false /* authorizedDatabases*/,
                                              scReply);
            // We need to copy the serialization context from the request to the reply object
            Reply reply(std::move(items), scReply);
            if (!nameOnly) {
                reply.setTotalSize(totalSize);
                reply.setTotalSizeMb(totalSize / (1024 * 1024));
            }

            return reply;
        }
    };
};
MONGO_REGISTER_COMMAND(CmdListDatabasesForAllTenants).forShard();
}  // namespace
}  // namespace mongo
