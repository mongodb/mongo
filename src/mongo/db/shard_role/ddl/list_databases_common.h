// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/database_name_util.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/ddl/list_databases_for_all_tenants_gen.h"
#include "mongo/db/shard_role/ddl/list_databases_gen.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_catalog/database_holder.h"
#include "mongo/db/shard_role/shard_catalog/db_raii.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/modules.h"

namespace mongo {

namespace [[MONGO_MOD_PARENT_PRIVATE]] list_databases {
using namespace std::literals::string_view_literals;
constexpr auto kName = "name"sv;

template <class CommandType>
std::unique_ptr<MatchExpression> getFilter(CommandType cmd,
                                           OperationContext* opCtx,
                                           NamespaceString ns) {
    if (auto filterObj = cmd.getFilter()) {
        // The collator is null because database metadata objects are compared using simple
        // binary comparison.
        auto expCtx = ExpressionContextBuilder{}.opCtx(opCtx).ns(ns).build();
        auto matcher =
            uassertStatusOK(MatchExpressionParser::parse(filterObj.get(), std::move(expCtx)));
        return matcher;
    }
    return std::unique_ptr<MatchExpression>{};
}
}  // namespace list_databases

namespace [[MONGO_MOD_PRIVATE]] list_databases {

// Initialize ListDatabasesForAllTenantsReplyItem reply item by setting the tenantId
void inline initializeItemWithTenantId(ListDatabasesForAllTenantsReplyItem& item,
                                       const DatabaseName& dbName) {
    item.setTenantId(dbName.tenantId());
}
// Do nothing if provided ListDatabasesReplyItem to satisy the compiler
void inline initializeItemWithTenantId(ListDatabasesReplyItem& item, const DatabaseName& dbName) {
    /* No-op */
}

int64_t sizeOnDiskForDb(OperationContext* opCtx,
                        const StorageEngine& storageEngine,
                        const DatabaseName& dbName);

template <typename ReplyItemType>
int64_t setReplyItems(OperationContext* opCtx,
                      const std::vector<DatabaseName>& dbNames,
                      std::vector<ReplyItemType>& items,
                      StorageEngine* storageEngine,
                      bool nameOnly,
                      const std::unique_ptr<MatchExpression>& filter,
                      bool setTenantId,
                      bool authorizedDatabases,
                      const SerializationContext serializationCtxt) {
    auto* as = AuthorizationSession::get(opCtx->getClient());

    const bool filterNameOnly = filter &&
        filter->getCategory() == MatchExpression::MatchCategory::kLeaf && filter->path() == kName;
    int64_t totalSize = 0;

    for (const auto& dbName : dbNames) {
        if (authorizedDatabases && !as->isAuthorizedForAnyActionOnAnyResourceInDB(dbName)) {
            // We don't have listDatabases on the cluster or find on this database.
            continue;
        }

        ReplyItemType item(DatabaseNameUtil::serialize(dbName, serializationCtxt));
        if (setTenantId) {
            initializeItemWithTenantId(item, dbName);
        }

        int64_t size = 0;
        if (!nameOnly) {
            // Filtering on name only should not require taking locks on filtered-out names.
            if (filterNameOnly && !exec::matcher::matchesBSON(filter.get(), item.toBSON())) {
                continue;
            }

            AutoGetDbForReadMaybeLockFree lockFreeReadBlock(opCtx, dbName);
            // The database could have been dropped since we called 'listDatabases()' originally.
            if (!DatabaseHolder::get(opCtx)->dbExists(opCtx, dbName)) {
                continue;
            }

            writeConflictRetry(opCtx, "sizeOnDisk", NamespaceString(dbName), [&] {
                size = sizeOnDiskForDb(opCtx, *storageEngine, dbName);
            });
            item.setSizeOnDisk(size);
            item.setEmpty(
                CollectionCatalog::get(opCtx)->getAllCollectionUUIDsFromDb(dbName).empty());
        }
        if (!filter || exec::matcher::matchesBSON(filter.get(), item.toBSON())) {
            totalSize += size;
            items.push_back(std::move(item));
        }
    }
    return totalSize;
}

}  // namespace list_databases
}  // namespace mongo
