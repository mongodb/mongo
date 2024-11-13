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


#include "mongo/s/router_role.h"

#include <memory>
#include <utility>

#include <boost/move/utility_core.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_version.h"
#include "mongo/s/sharding_state.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace sharding {
namespace router {

RouterBase::RouterBase(ServiceContext* service) : _service(service) {}

DBPrimaryRouter::DBPrimaryRouter(ServiceContext* service, const DatabaseName& db)
    : RouterBase(service), _dbName(db) {}

void DBPrimaryRouter::appendDDLRoutingTokenToCommand(const DatabaseType& dbt,
                                                     BSONObjBuilder* builder) {
    const auto& dbVersion = dbt.getVersion();
    if (!dbVersion.isFixed()) {
        BSONObjBuilder dbvBuilder(builder->subobjStart(DatabaseVersion::kDatabaseVersionField));
        dbVersion.serialize(&dbvBuilder);
    }
}

void DBPrimaryRouter::appendCRUDUnshardedRoutingTokenToCommand(const ShardId& shardId,
                                                               const DatabaseVersion& dbVersion,
                                                               BSONObjBuilder* builder) {
    if (!dbVersion.isFixed()) {
        BSONObjBuilder dbvBuilder(builder->subobjStart(DatabaseVersion::kDatabaseVersionField));
        dbVersion.serialize(&dbvBuilder);
    }
    ShardVersion::UNSHARDED().serialize(ShardVersion::kShardVersionField, builder);
}

CachedDatabaseInfo DBPrimaryRouter::_getRoutingInfo(OperationContext* opCtx) const {
    auto catalogCache = Grid::get(_service)->catalogCache();
    return uassertStatusOK(catalogCache->getDatabase(opCtx, _dbName));
}

void DBPrimaryRouter::_onException(RouteContext* context, Status s) {
    auto catalogCache = Grid::get(_service)->catalogCache();

    if (s == ErrorCodes::StaleDbVersion) {
        auto si = s.extraInfo<StaleDbRoutingVersion>();
        tassert(6375900, "StaleDbVersion must have extraInfo", si);
        tassert(6375901,
                str::stream() << "StaleDbVersion on unexpected database. Expected "
                              << _dbName.toStringForErrorMsg() << ", received "
                              << si->getDb().toStringForErrorMsg(),
                si->getDb() == _dbName);

        catalogCache->onStaleDatabaseVersion(si->getDb(), si->getVersionWanted());
    } else {
        uassertStatusOK(s);
    }

    if (++context->numAttempts > kMaxNumStaleVersionRetries) {
        uassertStatusOKWithContext(
            s,
            str::stream() << "Exceeded maximum number of " << kMaxNumStaleVersionRetries
                          << " retries attempting \'" << context->comment << "\'");
    } else {
        LOGV2_DEBUG(6375902,
                    3,
                    "Retrying database primary routing operation",
                    "attempt"_attr = context->numAttempts,
                    "comment"_attr = context->comment,
                    "status"_attr = s);
    }
}

CollectionRouterCommon::CollectionRouterCommon(
    ServiceContext* service, const std::vector<NamespaceString>& targetedNamespaces)
    : RouterBase(service), _targetedNamespaces(targetedNamespaces) {}

void CollectionRouterCommon::_onException(RouteContext* context, Status s) {
    auto catalogCache = Grid::get(_service)->catalogCache();

    const auto isNssInvolvedInRouting = [&](const NamespaceString& nss) {
        if (nss.isTimeseriesBucketsCollection() &&
            std::find(_targetedNamespaces.begin(),
                      _targetedNamespaces.end(),
                      nss.getTimeseriesViewNamespace()) != _targetedNamespaces.end()) {
            return true;
        }

        return std::find(_targetedNamespaces.begin(), _targetedNamespaces.end(), nss) !=
            _targetedNamespaces.end();
    };

    if (s == ErrorCodes::StaleDbVersion) {
        auto si = s.extraInfo<StaleDbRoutingVersion>();
        tassert(6375903, "StaleDbVersion must have extraInfo", si);
        catalogCache->onStaleDatabaseVersion(si->getDb(), si->getVersionWanted());
    } else if (s == ErrorCodes::StaleConfig) {
        auto si = s.extraInfo<StaleConfigInfo>();
        tassert(6375904, "StaleConfig must have extraInfo", si);

        if (!isNssInvolvedInRouting(si->getNss())) {
            uassertStatusOK(s);
        }

        catalogCache->invalidateShardOrEntireCollectionEntryForShardedCollection(
            si->getNss(), si->getVersionWanted(), si->getShardId());
    } else if (s == ErrorCodes::StaleEpoch) {
        if (auto si = s.extraInfo<StaleEpochInfo>()) {
            if (!isNssInvolvedInRouting(si->getNss())) {
                uassertStatusOK(s);
            }

            catalogCache->invalidateShardOrEntireCollectionEntryForShardedCollection(
                si->getNss(), si->getVersionWanted(), ShardId());
        }
    } else {
        uassertStatusOK(s);
    }

    if (++context->numAttempts > kMaxNumStaleVersionRetries) {
        uassertStatusOKWithContext(
            s,
            str::stream() << "Exceeded maximum number of " << kMaxNumStaleVersionRetries
                          << " retries attempting \'" << context->comment << "\'");
    } else {
        LOGV2_DEBUG(6375906,
                    3,
                    "Retrying collection routing operation",
                    "attempt"_attr = context->numAttempts,
                    "comment"_attr = context->comment,
                    "status"_attr = s);
    }
}

CollectionRoutingInfo CollectionRouterCommon::_getRoutingInfo(OperationContext* opCtx,
                                                              const NamespaceString& nss) {
    auto catalogCache = Grid::get(opCtx->getServiceContext())->catalogCache();
    // When in a multi-document transaction, allow getting routing info from the CatalogCache even
    // though locks may be held. The CatalogCache will throw CannotRefreshDueToLocksHeld if the
    // entry is not already cached.
    const auto allowLocks = opCtx->inMultiDocumentTransaction();
    return uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, nss, allowLocks));
}

void CollectionRouterCommon::appendCRUDRoutingTokenToCommand(const ShardId& shardId,
                                                             const CollectionRoutingInfo& cri,
                                                             BSONObjBuilder* builder) {
    if (cri.cm.getVersion(shardId) == ChunkVersion::UNSHARDED()) {
        // Need to add the database version as well.
        const auto& dbVersion = cri.cm.dbVersion();
        if (!dbVersion.isFixed()) {
            BSONObjBuilder dbvBuilder(builder->subobjStart(DatabaseVersion::kDatabaseVersionField));
            dbVersion.serialize(&dbvBuilder);
        }
    }
    cri.getShardVersion(shardId).serialize(ShardVersion::kShardVersionField, builder);
}

CollectionRouter::CollectionRouter(ServiceContext* service, NamespaceString nss)
    : CollectionRouterCommon(service, {std::move(nss)}) {}

MultiCollectionRouter::MultiCollectionRouter(ServiceContext* service,
                                             const std::vector<NamespaceString>& nssList)
    : CollectionRouterCommon(service, nssList) {}

bool MultiCollectionRouter::isAnyCollectionNotLocal(
    OperationContext* opCtx,
    const stdx::unordered_map<NamespaceString, CollectionRoutingInfo>& criMap) {
    auto* grid = Grid::get(opCtx->getServiceContext());
    // By definition, all collections in a non sharded deployment are local.
    if (!(grid->isInitialized() && grid->isShardingInitialized())) {
        return false;
    }

    const auto myShardId = ShardingState::get(opCtx)->shardId();
    bool anyCollectionNotLocal = false;

    // For each collection, figure out if it fully lives on this shard.
    for (const auto& nss : _targetedNamespaces) {
        const auto nssCri = criMap.find(nss);
        tassert(8322001,
                "Must be an entry in criMap for namespace " + nss.toStringForErrorMsg(),
                nssCri != criMap.end());

        const auto atClusterTime = repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime();
        const auto chunkManagerMaybeAtClusterTime = atClusterTime
            ? ChunkManager::makeAtTime(nssCri->second.cm, atClusterTime->asTimestamp())
            : nssCri->second.cm;

        bool isNssLocal = [&]() {
            if (chunkManagerMaybeAtClusterTime.isSharded()) {
                return false;
            } else if (chunkManagerMaybeAtClusterTime.isUnsplittable()) {
                return chunkManagerMaybeAtClusterTime.getMinKeyShardIdWithSimpleCollation() ==
                    myShardId;
            } else {
                // If collection is untracked, it is only local if this shard is the
                // dbPrimary shard.
                return chunkManagerMaybeAtClusterTime.dbPrimary() == myShardId;
            }
        }();

        if (!isNssLocal) {
            anyCollectionNotLocal = true;
        }
    }
    return anyCollectionNotLocal;
}
}  // namespace router
}  // namespace sharding
}  // namespace mongo
