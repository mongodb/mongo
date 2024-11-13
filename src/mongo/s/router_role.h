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

#include <string>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_id.h"
#include "mongo/s/catalog/type_database_gen.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/database_version.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace sharding {
namespace router {

class RouterBase {
protected:
    RouterBase(ServiceContext* service);

    struct RouteContext {
        const std::string comment;
        int numAttempts{0};
    };

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
        RouteContext context{comment.toString()};
        while (true) {
            auto cdb = _getRoutingInfo(opCtx);
            try {
                return callbackFn(opCtx, cdb);
            } catch (const DBException& ex) {
                _onException(&context, ex.toStatus());
            }
        }
    }

    static void appendDDLRoutingTokenToCommand(const DatabaseType& dbt, BSONObjBuilder* builder);

    static void appendCRUDUnshardedRoutingTokenToCommand(const ShardId& shardId,
                                                         const DatabaseVersion& dbVersion,
                                                         BSONObjBuilder* builder);

private:
    CachedDatabaseInfo _getRoutingInfo(OperationContext* opCtx) const;
    void _onException(RouteContext* context, Status s);

    DatabaseName _dbName;
};

/**
 * Class which contains logic common to routers which target one or more collections.
 */
class CollectionRouterCommon : public RouterBase {
protected:
    CollectionRouterCommon(ServiceContext* service,
                           const std::vector<NamespaceString>& routingNamespaces);

    static void appendCRUDRoutingTokenToCommand(const ShardId& shardId,
                                                const CollectionRoutingInfo& cri,
                                                BSONObjBuilder* builder);

    void _onException(RouteContext* context, Status s);
    CollectionRoutingInfo _getRoutingInfo(OperationContext* opCtx, const NamespaceString& nss);

    const std::vector<NamespaceString> _targetedNamespaces;
};

/**
 * This class should mostly be used for routing CRUD operations which need to have a view of the
 * entire routing table for a collection.
 */
class CollectionRouter : public CollectionRouterCommon {
public:
    CollectionRouter(ServiceContext* service, NamespaceString nss);

    template <typename F>
    auto route(OperationContext* opCtx, StringData comment, F&& callbackFn) {
        RouteContext context{comment.toString()};
        while (true) {
            auto cri = _getRoutingInfo(opCtx, _targetedNamespaces.front());
            try {
                return callbackFn(opCtx, cri);
            } catch (const DBException& ex) {
                _onException(&context, ex.toStatus());
            }
        }
    }
};

class MultiCollectionRouter : public CollectionRouterCommon {
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
        RouteContext context{comment.toString()};
        while (true) {
            stdx::unordered_map<NamespaceString, CollectionRoutingInfo> criMap;
            for (const auto& nss : _targetedNamespaces) {
                criMap.emplace(nss, _getRoutingInfo(opCtx, nss));
            }

            try {
                return callbackFn(opCtx, criMap);
            } catch (const DBException& ex) {
                _onException(&context, ex.toStatus());
            }
        }
    }
};

}  // namespace router
}  // namespace sharding
}  // namespace mongo
