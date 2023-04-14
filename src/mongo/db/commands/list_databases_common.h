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
#pragma once

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/list_databases_for_all_tenants_gen.h"
#include "mongo/db/commands/list_databases_gen.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/database_name_util.h"


namespace mongo {

namespace list_databases {

constexpr auto kName = "name"_sd;

// Initialize ListDatabasesForAllTenantsReplyItem reply item by setting the tenantId
void inline initializeItemWithTenantId(ListDatabasesForAllTenantsReplyItem& item,
                                       const DatabaseName& dbName) {
    item.setTenantId(dbName.tenantId());
}
// Do nothing if provided ListDatabasesReplyItem to satisy the compiler
void inline initializeItemWithTenantId(ListDatabasesReplyItem& item, const DatabaseName& dbName) {
    /* No-op */
}

template <class CommandType>
std::unique_ptr<MatchExpression> getFilter(CommandType cmd,
                                           OperationContext* opCtx,
                                           NamespaceString ns) {
    if (auto filterObj = cmd.getFilter()) {
        // The collator is null because database metadata objects are compared using simple
        // binary comparison.
        auto expCtx = make_intrusive<ExpressionContext>(
            opCtx, std::unique_ptr<CollatorInterface>(nullptr), ns);
        auto matcher =
            uassertStatusOK(MatchExpressionParser::parse(filterObj.get(), std::move(expCtx)));
        return matcher;
    }
    return std::unique_ptr<MatchExpression>{};
}

template <typename ReplyItemType>
int64_t setReplyItems(OperationContext* opCtx,
                      const std::vector<DatabaseName>& dbNames,
                      std::vector<ReplyItemType>& items,
                      StorageEngine* storageEngine,
                      bool nameOnly,
                      const std::unique_ptr<MatchExpression>& filter,
                      bool setTenantId,
                      bool authorizedDatabases) {
    auto* as = AuthorizationSession::get(opCtx->getClient());

    const bool filterNameOnly = filter &&
        filter->getCategory() == MatchExpression::MatchCategory::kLeaf && filter->path() == kName;
    int64_t totalSize = 0;

    for (const auto& dbName : dbNames) {
        if (authorizedDatabases &&
            !as->isAuthorizedForAnyActionOnAnyResourceInDB(dbName.toString())) {
            // We don't have listDatabases on the cluster or find on this database.
            continue;
        }

        // If setTenantId is true, always return the dbName without the tenantId
        ReplyItemType item(setTenantId ? dbName.db().toString()
                                       : DatabaseNameUtil::serialize(dbName));
        if (setTenantId) {
            initializeItemWithTenantId(item, dbName);
        }

        int64_t size = 0;
        if (!nameOnly) {
            // Filtering on name only should not require taking locks on filtered-out names.
            if (filterNameOnly && !filter->matchesBSON(item.toBSON())) {
                continue;
            }

            AutoGetDbForReadMaybeLockFree lockFreeReadBlock(opCtx, dbName);
            // The database could have been dropped since we called 'listDatabases()' originally.
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
    return totalSize;
}

}  // namespace list_databases
}  // namespace mongo
