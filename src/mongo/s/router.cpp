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


#include "mongo/s/router.h"

#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/s/stale_exception.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace sharding {
namespace router {

RouterBase::RouterBase(ServiceContext* service) : _service(service) {}

DBPrimaryRouter::DBPrimaryRouter(ServiceContext* service, StringData db)
    : RouterBase(service), _db(db.toString()) {}

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
    return uassertStatusOK(catalogCache->getDatabase(opCtx, _db));
}

void DBPrimaryRouter::_onException(RouteContext* context, Status s) {
    if (++context->numAttempts > kMaxNumStaleVersionRetries) {
        uassertStatusOKWithContext(
            s,
            str::stream() << "Exceeded maximum number of " << kMaxNumStaleVersionRetries
                          << " retries attempting \'" << context->comment << "\'");
    } else {
        LOGV2_DEBUG(637590,
                    3,
                    "Retrying {description}. Got error: {status}",
                    "description"_attr = context->comment,
                    "status"_attr = s);
    }

    auto catalogCache = Grid::get(_service)->catalogCache();

    if (s == ErrorCodes::StaleDbVersion) {
        auto si = s.extraInfo<StaleDbRoutingVersion>();
        invariant(si);
        invariant(si->getDb() == _db,
                  str::stream() << "StaleDbVersion on unexpected database. Expected " << _db
                                << ", received " << si->getDb());

        catalogCache->onStaleDatabaseVersion(si->getDb(), si->getVersionWanted());
    } else {
        uassertStatusOK(s);
    }
}

CollectionRouter::CollectionRouter(ServiceContext* service, NamespaceString nss)
    : RouterBase(service), _nss(std::move(nss)) {}

void CollectionRouter::appendCRUDRoutingTokenToCommand(const ShardId& shardId,
                                                       const ChunkManager& cm,
                                                       BSONObjBuilder* builder) {
    auto chunkVersion(cm.getVersion(shardId));

    if (chunkVersion == ChunkVersion::UNSHARDED()) {
        // Need to add the database version as well
        const auto& dbVersion = cm.dbVersion();
        if (!dbVersion.isFixed()) {
            BSONObjBuilder dbvBuilder(builder->subobjStart(DatabaseVersion::kDatabaseVersionField));
            dbVersion.serialize(&dbvBuilder);
        }
    }
    ShardVersion(chunkVersion, CollectionIndexes(chunkVersion, boost::none))
        .serialize(ShardVersion::kShardVersionField, builder);
}

ChunkManager CollectionRouter::_getRoutingInfo(OperationContext* opCtx) const {
    auto catalogCache = Grid::get(_service)->catalogCache();
    return uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, _nss));
}

void CollectionRouter::_onException(RouteContext* context, Status s) {
    if (++context->numAttempts > kMaxNumStaleVersionRetries) {
        uassertStatusOKWithContext(
            s,
            str::stream() << "Exceeded maximum number of " << kMaxNumStaleVersionRetries
                          << " retries attempting \'" << context->comment << "\'");
    } else {
        LOGV2_DEBUG(637591,
                    3,
                    "Retrying {description}. Got error: {status}",
                    "description"_attr = context->comment,
                    "status"_attr = s);
    }

    auto catalogCache = Grid::get(_service)->catalogCache();

    if (s.isA<ErrorCategory::StaleShardVersionError>()) {
        if (auto si = s.extraInfo<StaleConfigInfo>()) {
            invariant(si->getNss() == _nss,
                      str::stream() << "StaleConfig on unexpected namespace. Expected " << _nss
                                    << ", received " << si->getNss());
            catalogCache->invalidateShardOrEntireCollectionEntryForShardedCollection(
                _nss, si->getVersionWanted(), si->getShardId());
        } else {
            catalogCache->invalidateCollectionEntry_LINEARIZABLE(_nss);
        }
    } else if (s == ErrorCodes::StaleDbVersion) {
        auto si = s.extraInfo<StaleDbRoutingVersion>();
        invariant(si);
        invariant(si->getDb() == _nss.db(),
                  str::stream() << "StaleDbVersion on unexpected database. Expected " << _nss.db()
                                << ", received " << si->getDb());

        catalogCache->onStaleDatabaseVersion(si->getDb(), si->getVersionWanted());
    } else {
        uassertStatusOK(s);
    }
}

}  // namespace router
}  // namespace sharding
}  // namespace mongo
