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
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/router_role/routing_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <string>

namespace mongo {
namespace sharding {
namespace router {

class MONGO_MOD_PRIVATE RouterBase {
protected:
    RouterBase(CatalogCache* catalogCache);

    struct RoutingRetryInfo {
        const std::string comment;
        int numAttempts{0};
    };

    void _initTxnRouterIfNeeded(OperationContext* opCtx);

    CatalogCache* const _catalogCache;
};

// Both the router classes below declare the scope in which their 'route' method is executed as a
// router for the relevant database or collection. These classes are the only way to obtain routing
// information for a given entry.

/**
 * This class should mostly be used for routing of DDL operations which need to be coordinated from
 * the primary shard of the database.
 */
class MONGO_MOD_PUBLIC DBPrimaryRouter final : public RouterBase {
public:
    DBPrimaryRouter(ServiceContext* service, const DatabaseName& db);

    template <typename F>
    auto route(OperationContext* opCtx, StringData comment, F&& callbackFn) {
        RoutingRetryInfo retryInfo{std::string{comment}};
        _initTxnRouterIfNeeded(opCtx);

        while (true) {
            auto cdb = _createDbIfRequestedAndGetRoutingInfo(opCtx);
            try {
                return callbackFn(opCtx, cdb);
            } catch (const DBException& ex) {
                _onException(opCtx, &retryInfo, ex.toStatus());
            }
        }
    }

    /**
     * Enables implicit database creation when an operation is routed through this DBPrimaryRouter
     * instance.
     * Handles concurrent database drops: if the command throws a NamespaceNotFound error because
     * the database was concurrently dropped, the database will be recreated and the operation will
     * be automatically retried.
     * If a suggestedPrimaryId is specified, the database will be created on the given shard only
     * if the database doesn't exist yet. If the database is already created, the suggestedPrimaryId
     * parameter will be ignored.
     */
    void createDbImplicitlyOnRoute(
        const boost::optional<ShardId>& suggestedPrimaryId = boost::none) {
        _createDbImplicitly = true;
        _suggestedPrimaryId = suggestedPrimaryId;
    }

    static void appendDDLRoutingTokenToCommand(const DatabaseType& dbt, BSONObjBuilder* builder);

    static void appendCRUDUnshardedRoutingTokenToCommand(const ShardId& shardId,
                                                         const DatabaseVersion& dbVersion,
                                                         BSONObjBuilder* builder);

private:
    CachedDatabaseInfo _getRoutingInfo(OperationContext* opCtx) const;
    CachedDatabaseInfo _createDbIfRequestedAndGetRoutingInfo(OperationContext* opCtx) const;
    void _onException(OperationContext* opCtx, RoutingRetryInfo* retryInfo, Status s);

    DatabaseName _dbName;

    bool _createDbImplicitly{false};
    boost::optional<ShardId> _suggestedPrimaryId;
};

/**
 * Class which contains logic common to routers which target one or more collections.
 */
class MONGO_MOD_PRIVATE CollectionRouterCommon : public RouterBase {
protected:
    CollectionRouterCommon(CatalogCache* catalogCache,
                           const std::vector<NamespaceString>& routingNamespaces);

    static void appendCRUDRoutingTokenToCommand(const ShardId& shardId,
                                                const CollectionRoutingInfo& cri,
                                                BSONObjBuilder* builder);

    void _onException(OperationContext* opCtx, RoutingRetryInfo* retryInfo, Status s);
    CollectionRoutingInfo _getRoutingInfo(OperationContext* opCtx, const NamespaceString& nss);

    const std::vector<NamespaceString> _targetedNamespaces;
};

/**
 * This class should mostly be used for routing CRUD operations which need to have a view of the
 * entire routing table for a collection.
 */
class MONGO_MOD_PUBLIC CollectionRouter final : public CollectionRouterCommon {
public:
    CollectionRouter(ServiceContext* service, NamespaceString nss);

    // TODO SERVER-95927: Remove this constructor.
    CollectionRouter(CatalogCache* catalogCache, NamespaceString nss);

    template <typename F>
    auto route(OperationContext* opCtx, StringData comment, F&& callbackFn) {
        return _routeImpl(opCtx, comment, [&] {
            auto cri = _createDbIfRequestedAndGetRoutingInfo(opCtx);
            return callbackFn(opCtx, cri);
        });
    }

    template <typename F>
    auto routeWithRoutingContext(OperationContext* opCtx, StringData comment, F&& callbackFn) {
        return _routeImpl(opCtx, comment, [&] {
            RoutingContext routingCtx = _createDbIfRequestedAndGetRoutingContext(opCtx);
            return routing_context_utils::runAndValidate(
                routingCtx, [&](RoutingContext& ctx) { return callbackFn(opCtx, ctx); });
        });
    }

    /**
     * Enables implicit database creation when an operation is routed through this CollectionRouter
     * instance.
     * Handles concurrent database drops: if the command throws a NamespaceNotFound error because
     * the database was concurrently dropped, the database will be recreated and the operation will
     * be automatically retried.
     */
    void createDbImplicitlyOnRoute(
        const boost::optional<ShardId>& suggestedPrimaryId = boost::none) {
        _createDbImplicitly = true;
        _suggestedPrimaryId = suggestedPrimaryId;
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

    RoutingContext _getRoutingContext(OperationContext* opCtx);

    RoutingContext _createDbIfRequestedAndGetRoutingContext(OperationContext* opCtx);
    CollectionRoutingInfo _createDbIfRequestedAndGetRoutingInfo(OperationContext* opCtx);

    bool _createDbImplicitly{false};
    boost::optional<ShardId> _suggestedPrimaryId;
};

class MONGO_MOD_PUBLIC MultiCollectionRouter final : public CollectionRouterCommon {
public:
    MultiCollectionRouter(ServiceContext* service, const std::vector<NamespaceString>& nssList);

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
