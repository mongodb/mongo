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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include <boost/none_t.hpp>

#include "mongo/platform/basic.h"

#include "mongo/s/client/shard_local.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

ShardLocal::ShardLocal(const ShardId& id) : Shard(id), _rsLocalClient() {
    // Currently ShardLocal only works for config servers. If we ever start using ShardLocal on
    // shards we'll need to consider how to handle shards.
    invariant(serverGlobalParams.clusterRole == ClusterRole::ConfigServer);
}

const ConnectionString ShardLocal::getConnString() const {
    return repl::ReplicationCoordinator::get(getGlobalServiceContext())
        ->getConfig()
        .getConnectionString();
}

std::shared_ptr<RemoteCommandTargeter> ShardLocal::getTargeter() const {
    MONGO_UNREACHABLE;
};

const ConnectionString ShardLocal::originalConnString() const {
    // Return the local connection string here as this method is only used for updating the
    // ShardRegistry and we don't need a mapping from hosts in the replica set config to the shard
    // for local shards.
    return ConnectionString::forLocal();
}

void ShardLocal::updateReplSetMonitor(const HostAndPort& remoteHost,
                                      const Status& remoteCommandStatus) {
    MONGO_UNREACHABLE;
}

void ShardLocal::updateLastCommittedOpTime(LogicalTime lastCommittedOpTime) {
    MONGO_UNREACHABLE;
}

LogicalTime ShardLocal::getLastCommittedOpTime() const {
    MONGO_UNREACHABLE;
}

std::string ShardLocal::toString() const {
    return getId().toString() + ":<local>";
}

bool ShardLocal::isRetriableError(ErrorCodes::Error code, RetryPolicy options) {
    if (options == RetryPolicy::kNoRetry) {
        return false;
    }

    if (options == RetryPolicy::kIdempotent) {
        return code == ErrorCodes::WriteConcernFailed;
    } else {
        invariant(options == RetryPolicy::kNotIdempotent);
        return false;
    }
}

StatusWith<Shard::CommandResponse> ShardLocal::_runCommand(OperationContext* opCtx,
                                                           const ReadPreferenceSetting& unused,
                                                           StringData dbName,
                                                           Milliseconds maxTimeMSOverrideUnused,
                                                           const BSONObj& cmdObj) {
    return _rsLocalClient.runCommandOnce(opCtx, dbName, cmdObj);
}

StatusWith<Shard::QueryResponse> ShardLocal::_runExhaustiveCursorCommand(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    StringData dbName,
    Milliseconds maxTimeMSOverride,
    const BSONObj& cmdObj) {
    MONGO_UNREACHABLE;
}

StatusWith<Shard::QueryResponse> ShardLocal::_exhaustiveFindOnConfig(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    const repl::ReadConcernLevel& readConcernLevel,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& sort,
    boost::optional<long long> limit) {
    return _rsLocalClient.queryOnce(opCtx, readPref, readConcernLevel, nss, query, sort, limit);
}

Status ShardLocal::createIndexOnConfig(OperationContext* opCtx,
                                       const NamespaceString& ns,
                                       const BSONObj& keys,
                                       bool unique) {
    invariant(ns.db() == "config" || ns.db() == "admin");

    try {
        AutoGetOrCreateDb autoDb(opCtx, ns.db(), MODE_IX);
        AutoGetCollection autoColl(opCtx, ns, MODE_X);
        auto collection = autoColl.getCollection();
        if (!collection) {
            CollectionOptions options;
            options.uuid = UUID::gen();
            writeConflictRetry(opCtx, "ShardLocal::createIndexOnConfig", ns.ns(), [&] {
                WriteUnitOfWork wunit(opCtx);
                auto db = autoDb.getDb();
                collection = db->createCollection(opCtx, ns, options);
                invariant(collection,
                          str::stream() << "Failed to create collection " << ns.ns()
                                        << " in config database for indexes: " << keys);
                wunit.commit();
            });
        }
        auto indexCatalog = collection->getIndexCatalog();
        IndexSpec index;
        index.addKeys(keys);
        index.unique(unique);
        index.version(int(IndexDescriptor::kLatestIndexVersion));
        auto removeIndexBuildsToo = false;
        auto indexSpecs = indexCatalog->removeExistingIndexes(
            opCtx,
            uassertStatusOK(
                collection->addCollationDefaultsToIndexSpecsForCreate(opCtx, {index.toBSON()})),
            removeIndexBuildsToo);

        if (indexSpecs.empty()) {
            return Status::OK();
        }

        auto fromMigrate = false;
        if (!collection->isEmpty(opCtx)) {
            // We typically create indexes on config/admin collections for sharding while setting up
            // a sharded cluster, so we do not expect to see data in the collection.
            // Therefore, it is ok to log this index build.
            LOGV2(5173300,
                  "Creating index on sharding collection with existing data",
                  "ns"_attr = ns,
                  "uuid"_attr = collection->uuid(),
                  "index"_attr = indexSpecs[0]);
            auto indexConstraints = IndexBuildsManager::IndexConstraints::kEnforce;
            IndexBuildsCoordinator::get(opCtx)->createIndexes(
                opCtx, collection->uuid(), indexSpecs, indexConstraints, fromMigrate);
        } else {
            writeConflictRetry(opCtx, "ShardLocal::createIndexOnConfig", ns.ns(), [&] {
                WriteUnitOfWork wunit(opCtx);
                IndexBuildsCoordinator::get(opCtx)->createIndexesOnEmptyCollection(
                    opCtx, collection->uuid(), indexSpecs, fromMigrate);
                wunit.commit();
            });
        }
    } catch (const DBException& e) {
        return e.toStatus();
    }

    return Status::OK();
}

void ShardLocal::runFireAndForgetCommand(OperationContext* opCtx,
                                         const ReadPreferenceSetting& readPref,
                                         const std::string& dbName,
                                         const BSONObj& cmdObj) {
    MONGO_UNREACHABLE;
}

}  // namespace mongo
