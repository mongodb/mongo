// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/router_role/routing_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>

namespace mongo {
namespace sharding {
namespace router {

class [[MONGO_MOD_PRIVATE]] RouterBase {
protected:
    RouterBase(OperationContext* opCtx, CatalogCache* catalogCache);

    struct RoutingRetryInfo {
        const std::string comment;
        int numAttempts{0};
    };

    void _initTxnRouterIfNeeded();

    // Arms the StaleConfig retry counter on the OperationContext (sets it to 0 if not already set)
    // so that versioned requests issued by this router advertise that a protocol-aware router is
    // driving the operation. Must be called once before the routing retry loop so that retries
    // (which increment the counter in _onException) are not reset.
    void _armStaleConfigRetryAttemptTracking();

    OperationContext* const _opCtx;
    CatalogCache* const _catalogCache;
};

// Both the router classes below declare the scope in which their 'route' method is executed as a
// router for the relevant database or collection. These classes are the only way to obtain routing
// information for a given entry.

/**
 * This class should mostly be used for routing of DDL operations which need to be coordinated from
 * the primary shard of the database.
 */
class [[MONGO_MOD_PUBLIC]] DBPrimaryRouter final : public RouterBase {
public:
    DBPrimaryRouter(OperationContext* opCtx, const DatabaseName& db);

    template <typename F>
    auto route(std::string_view comment, F&& callbackFn) {
        RoutingRetryInfo retryInfo{std::string{comment}};
        _initTxnRouterIfNeeded();
        _armStaleConfigRetryAttemptTracking();

        while (true) {
            auto cdb = _createDbIfRequestedAndGetRoutingInfo();
            try {
                return callbackFn(_opCtx, cdb);
            } catch (const DBException& ex) {
                _onException(&retryInfo, ex.toStatus());
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
    CachedDatabaseInfo _getRoutingInfo() const;
    CachedDatabaseInfo _createDbIfRequestedAndGetRoutingInfo() const;
    void _onException(RoutingRetryInfo* retryInfo, Status s);

    DatabaseName _dbName;

    bool _createDbImplicitly{false};
    boost::optional<ShardId> _suggestedPrimaryId;
};

/**
 * Class which contains logic common to routers which target one or more collections.
 */
class [[MONGO_MOD_PRIVATE]] CollectionRouterCommon : public RouterBase {
protected:
    CollectionRouterCommon(OperationContext* opCtx,
                           CatalogCache* catalogCache,
                           const std::vector<NamespaceString>& routingNamespaces);

    static void appendCRUDRoutingTokenToCommand(const ShardId& shardId,
                                                const CollectionRoutingInfo& cri,
                                                BSONObjBuilder* builder);

    void _onException(RoutingRetryInfo* retryInfo, Status s);
    CollectionRoutingInfo _getRoutingInfo(const NamespaceString& nss);

    const std::vector<NamespaceString> _targetedNamespaces;
};

/**
 * This class should mostly be used for routing CRUD operations which need to have a view of the
 * entire routing table for a collection.
 */
class [[MONGO_MOD_PUBLIC]] CollectionRouter final : public CollectionRouterCommon {
public:
    CollectionRouter(OperationContext* opCtx, NamespaceString nss);

    // TODO SERVER-95927: Remove this constructor.
    CollectionRouter(OperationContext* opCtx, CatalogCache* catalogCache, NamespaceString nss);

    template <typename F>
    auto route(std::string_view comment, F&& callbackFn) {
        return _routeImpl(comment, [&] {
            auto cri = _createDbIfRequestedAndGetRoutingInfo();
            return callbackFn(_opCtx, cri);
        });
    }

    template <typename F>
    auto routeWithRoutingContext(std::string_view comment, F&& callbackFn) {
        return _routeImpl(comment, [&] {
            RoutingContext routingCtx = _createDbIfRequestedAndGetRoutingContext();
            return routing_context_utils::runAndValidate(
                routingCtx, [&](RoutingContext& ctx) { return callbackFn(_opCtx, ctx); });
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
    auto _routeImpl(std::string_view comment, F&& work) {
        RoutingRetryInfo retryInfo{std::string{comment}};
        _initTxnRouterIfNeeded();
        _armStaleConfigRetryAttemptTracking();

        while (true) {
            try {
                return std::forward<F>(work)();
            } catch (const DBException& ex) {
                _onException(&retryInfo, ex.toStatus());
            }
        }
    }

    RoutingContext _getRoutingContext();

    RoutingContext _createDbIfRequestedAndGetRoutingContext();
    CollectionRoutingInfo _createDbIfRequestedAndGetRoutingInfo();

    bool _createDbImplicitly{false};
    boost::optional<ShardId> _suggestedPrimaryId;
};

class [[MONGO_MOD_PUBLIC]] MultiCollectionRouter final : public CollectionRouterCommon {
public:
    MultiCollectionRouter(OperationContext* opCtx, const std::vector<NamespaceString>& nssList);

    /**
     * Member function which discerns whether any of the namespaces in 'routingNamespaces' are not
     * local.
     */
    bool isAnyCollectionNotLocal(
        OperationContext*,
        const stdx::unordered_map<NamespaceString, CollectionRoutingInfo>& criMap);


    template <typename F>
    auto route(std::string_view comment, F&& callbackFn) {
        RoutingRetryInfo retryInfo{std::string{comment}};
        _initTxnRouterIfNeeded();
        _armStaleConfigRetryAttemptTracking();

        while (true) {
            stdx::unordered_map<NamespaceString, CollectionRoutingInfo> criMap;
            for (const auto& nss : _targetedNamespaces) {
                criMap.emplace(nss, _getRoutingInfo(nss));
            }

            try {
                return callbackFn(_opCtx, criMap);
            } catch (const DBException& ex) {
                _onException(&retryInfo, ex.toStatus());
            }
        }
    }
};

/**
 * Creates all of the databases referenced by 'nssList' (if they don't already exist), and then
 * creates and returns a RoutingContext that can be used for targeting write ops.
 */
[[MONGO_MOD_PUBLIC]] std::unique_ptr<RoutingContext> createDatabasesAndGetRoutingCtx(
    OperationContext* opCtx,
    const std::vector<NamespaceString>& nssList,
    bool checkTimeseriesBucketsNss,
    bool refresh);

}  // namespace router
}  // namespace sharding
}  // namespace mongo
