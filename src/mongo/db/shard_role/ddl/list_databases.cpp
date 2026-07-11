// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/shuffle_list_command_results.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/ddl/list_databases_common.h"
#include "mongo/db/shard_role/ddl/list_databases_gen.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog_helper.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/serialization_context.h"

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace list_databases {
int64_t sizeOnDiskForDb(OperationContext* opCtx,
                        const StorageEngine& storageEngine,
                        const DatabaseName& dbName) {
    int64_t size = 0;

    if (opCtx->isLockFreeReadsOp()) {
        auto collectionCatalog = CollectionCatalog::get(opCtx);
        for (auto&& coll : collectionCatalog->range(dbName)) {
            size += coll->sizeOnDisk(opCtx, storageEngine);
        }
    } else {
        catalog::forEachCollectionFromDb(opCtx, dbName, MODE_IS, [&](const Collection* coll) {
            size += coll->sizeOnDisk(opCtx, storageEngine);
            return true;
        });
    };

    return size;
}
}  // namespace list_databases

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
            {
                // Read lock free through a consistent in-memory catalog and storage snapshot.
                AutoReadLockFree lockFreeReadBlock(opCtx);
                auto catalog = CollectionCatalog::get(opCtx);

                CurOpFailpointHelpers::waitWhileFailPointEnabled(
                    &hangBeforeListDatabases, opCtx, "hangBeforeListDatabases", []() {});
                dbNames = catalog->getAllConsistentDbNamesForTenant(opCtx, tenantId);
            }

            shuffleListCommandResults.execute([&](const auto&) {
                std::random_device rd;
                std::mt19937 g(rd());
                std::shuffle(dbNames.begin(), dbNames.end(), g);
            });

            std::vector<ListDatabasesReplyItem> items;
            SerializationContext scReply =
                SerializationContext::stateCommandReply(cmd.getSerializationContext());
            if (gMultitenancySupport && !tenantId) {
                // During the multitenancy upgrade process a mongod might receive listDatabases from
                // a non-multitenant node (with prefix and without token) during initial sync.
                scReply.setPrefixState(true);
            }
            int64_t totalSize =
                list_databases::setReplyItems(opCtx,
                                              dbNames,
                                              items,
                                              getGlobalServiceContext()->getStorageEngine(),
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
