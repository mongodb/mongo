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

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/list_databases_gen.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/database_name.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"

namespace mongo {

// XXX: remove and put into storage api
std::intmax_t dbSize(const std::string& database);

namespace {

// Failpoint which causes to hang "listDatabases" cmd after acquiring global lock in IS mode.
MONGO_FAIL_POINT_DEFINE(hangBeforeListDatabases);

constexpr auto kName = "name"_sd;
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

    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;

        bool supportsWriteConcern() const final {
            return false;
        }

        void doCheckAuthorization(OperationContext*) const final {}

        NamespaceString ns() const final {
            return NamespaceString(request().getDbName(), "");
        }

        ListDatabasesReply typedRun(OperationContext* opCtx) final {
            CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);
            auto* as = AuthorizationSession::get(opCtx->getClient());
            auto cmd = request();

            // {nameOnly: bool} - default false.
            const bool nameOnly = cmd.getNameOnly();

            // {authorizedDatabases: bool} - Dynamic default based on permissions.
            const bool authorizedDatabases = ([as](const boost::optional<bool>& authDB) {
                const bool mayListAllDatabases = as->isAuthorizedForActionsOnResource(
                    ResourcePattern::forClusterResource(), ActionType::listDatabases);

                if (authDB) {
                    uassert(ErrorCodes::Unauthorized,
                            "Insufficient permissions to list all databases",
                            authDB.get() || mayListAllDatabases);
                    return authDB.get();
                }

                // By default, list all databases if we can, otherwise
                // only those we're allowed to find on.
                return !mayListAllDatabases;
            })(cmd.getAuthorizedDatabases());

            // {filter: matchExpression}.
            std::unique_ptr<MatchExpression> filter;
            if (auto filterObj = cmd.getFilter()) {
                // The collator is null because database metadata objects are compared using simple
                // binary comparison.
                auto expCtx = make_intrusive<ExpressionContext>(
                    opCtx, std::unique_ptr<CollatorInterface>(nullptr), ns());
                auto matcher = uassertStatusOK(
                    MatchExpressionParser::parse(filterObj.get(), std::move(expCtx)));
                filter = std::move(matcher);
            }

            std::vector<DatabaseName> dbNames;
            StorageEngine* storageEngine = getGlobalServiceContext()->getStorageEngine();
            {
                Lock::GlobalLock lk(opCtx, MODE_IS);
                CurOpFailpointHelpers::waitWhileFailPointEnabled(
                    &hangBeforeListDatabases, opCtx, "hangBeforeListDatabases", []() {});
                dbNames = storageEngine->listDatabases();
            }

            std::vector<ListDatabasesReplyItem> items;

            const bool filterNameOnly = filter &&
                filter->getCategory() == MatchExpression::MatchCategory::kLeaf &&
                filter->path() == kName;
            long long totalSize = 0;
            for (const auto& dbName : dbNames) {
                if (authorizedDatabases &&
                    !as->isAuthorizedForAnyActionOnAnyResourceInDB(dbName.toString())) {
                    // We don't have listDatabases on the cluster or find on this database.
                    continue;
                }

                ListDatabasesReplyItem item(dbName.db());

                long long size = 0;
                if (!nameOnly) {
                    // Filtering on name only should not require taking locks on filtered-out names.
                    if (filterNameOnly && !filter->matchesBSON(item.toBSON())) {
                        continue;
                    }

                    AutoGetDbForReadMaybeLockFree lockFreeReadBlock(opCtx, dbName);
                    // The database could have been dropped since we called 'listDatabases()' above.
                    if (!DatabaseHolder::get(opCtx)->dbExists(opCtx, dbName)) {
                        continue;
                    }

                    writeConflictRetry(opCtx, "sizeOnDisk", dbName.toString(), [&] {
                        size = storageEngine->sizeOnDiskForDb(opCtx, dbName);
                    });
                    item.setSizeOnDisk(size);
                    item.setEmpty(
                        CollectionCatalog::get(opCtx)->getAllCollectionUUIDsFromDb(dbName).empty());
                }
                if (!filter || filter->matchesBSON(item.toBSON())) {
                    totalSize += size;
                    items.push_back(std::move(item));
                }
            }

            ListDatabasesReply reply(items);
            if (!nameOnly) {
                reply.setTotalSize(totalSize);
                reply.setTotalSizeMb(totalSize / (1024 * 1024));
            }

            return reply;
        }
    };
} cmdListDatabases;
}  // namespace
}  // namespace mongo
