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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/router_role_api/routing_context.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/util/assert_util.h"

#include <string>

namespace mongo {
namespace sharding {
namespace router {

class RouterBase {
protected:
    RouterBase(ServiceContext* service);

    struct RoutingRetryInfo {
        const std::string comment;
        int numAttempts{0};
    };

    void _initTxnRouterIfNeeded(OperationContext* opCtx);

    ServiceContext* const _service;
};

// Both the router classes below declare the scope in which their 'route' method is executed as a
// router for the relevant database or collection. These classes are the only way to obtain routing
// information for a given entry.

/**
 * This class should mostly be used for routing of DDL operations which need to be coordinated from
 * the primary shard of the database.
 */
class DBPrimaryRouter : public RouterBase {
public:
    DBPrimaryRouter(ServiceContext* service, const DatabaseName& db);

    template <typename F>
    auto route(OperationContext* opCtx, StringData comment, F&& callbackFn) {
        RoutingRetryInfo retryInfo{std::string{comment}};
        _initTxnRouterIfNeeded(opCtx);

        while (true) {
            auto cdb = _getRoutingInfo(opCtx);
            try {
                return callbackFn(opCtx, cdb);
            } catch (const DBException& ex) {
                _onException(opCtx, &retryInfo, ex.toStatus());
            }
        }
    }

    static void appendDDLRoutingTokenToCommand(const DatabaseType& dbt, BSONObjBuilder* builder);

    static void appendCRUDUnshardedRoutingTokenToCommand(const ShardId& shardId,
                                                         const DatabaseVersion& dbVersion,
                                                         BSONObjBuilder* builder);

private:
    CachedDatabaseInfo _getRoutingInfo(OperationContext* opCtx) const;
    void _onException(OperationContext* opCtx, RoutingRetryInfo* retryInfo, Status s);

    DatabaseName _dbName;
};

/**
 * Class which contains logic common to routers which target one or more collections.
 */
class CollectionRouterCommon : public RouterBase {
protected:
    CollectionRouterCommon(ServiceContext* service,
                           const std::vector<NamespaceString>& routingNamespaces,
                           bool retryOnStaleShard);

    static void appendCRUDRoutingTokenToCommand(const ShardId& shardId,
                                                const CollectionRoutingInfo& cri,
                                                BSONObjBuilder* builder);

    void _onException(OperationContext* opCtx, RoutingRetryInfo* retryInfo, Status s);
    CollectionRoutingInfo _getRoutingInfo(OperationContext* opCtx, const NamespaceString& nss);

    const std::vector<NamespaceString> _targetedNamespaces;

    // TODO: SERVER-77402: Remove this.
    // Controls whether this loop will retry when a StaleDatabaseVersion/StaleShardVersion
    // indicates that the shard (rather than the router) was stale. When 'true', RouterLoop will
    // retry; when 'false', the exception will be rethrown verbatim.
    // It only makes sense to retry when the enclosed RouterRole operation results in a shardRole
    // invocation that ensures that the shard will refresh its metadata in case it was stale. This
    // is the case for remote command execution, where the shard's ServiceEntryPoint ensures that.
    // This flag must be set to false for operations that re-enter into the ShardRole without a
    // ShardRole refresh loop.
    const bool _retryOnStaleShard{true};
};

/**
 * This class should mostly be used for routing CRUD operations which need to have a view of the
 * entire routing table for a collection.
 */
class CollectionRouter : public CollectionRouterCommon {
public:
    CollectionRouter(ServiceContext* service, NamespaceString nss, bool retryOnStaleShard = true);

    template <typename F>
    auto route(OperationContext* opCtx, StringData comment, F&& callbackFn) {
        return _routeImpl(opCtx, comment, [&] {
            auto cri = _getRoutingInfo(opCtx, _targetedNamespaces.front());
            return callbackFn(opCtx, cri);
        });
    }

    template <typename F>
    auto routeWithRoutingContext(OperationContext* opCtx, StringData comment, F&& callbackFn) {
        return _routeImpl(opCtx, comment, [&] {
            // When in a multi-document transaction, allow getting routing info from the
            // CatalogCache even though locks may be held. The CatalogCache will throw
            // CannotRefreshDueToLocksHeld if the entry is not already cached.
            const auto allowLocks = opCtx->inMultiDocumentTransaction() &&
                shard_role_details::getLocker(opCtx)->isLocked();

            RoutingContext routingCtx(opCtx, _targetedNamespaces, allowLocks);
            return routing_context_utils::runAndValidate(
                routingCtx, [&](RoutingContext& ctx) { return callbackFn(opCtx, ctx); });
        });
    }

private:
    template <typename F>
    auto _routeImpl(OperationContext* opCtx, StringData comment, F&& work) {
        RoutingRetryInfo retryInfo{std::string{comment}};
        _initTxnRouterIfNeeded(opCtx);

        while (true) {
            try {
                return std::forward<F>(work)();
            } catch (const DBException& ex) {
                _onException(opCtx, &retryInfo, ex.toStatus());
            }
        }
    }
};

class MultiCollectionRouter : public CollectionRouterCommon {
public:
    MultiCollectionRouter(ServiceContext* service,
                          const std::vector<NamespaceString>& nssList,
                          bool retryOnStaleShard = true);

    /**
     * Member function which discerns whether any of the namespaces in 'routingNamespaces' are not
     * local.
     */
    bool isAnyCollectionNotLocal(
        OperationContext* opCtx,
        const stdx::unordered_map<NamespaceString, CollectionRoutingInfo>& criMap);


    template <typename F>
    auto route(OperationContext* opCtx, StringData comment, F&& callbackFn) {
        RoutingRetryInfo retryInfo{std::string{comment}};
        _initTxnRouterIfNeeded(opCtx);

        while (true) {
            stdx::unordered_map<NamespaceString, CollectionRoutingInfo> criMap;
            for (const auto& nss : _targetedNamespaces) {
                criMap.emplace(nss, _getRoutingInfo(opCtx, nss));
            }

            try {
                return callbackFn(opCtx, criMap);
            } catch (const DBException& ex) {
                _onException(opCtx, &retryInfo, ex.toStatus());
            }
        }
    }
};

}  // namespace router
}  // namespace sharding
}  // namespace mongo
