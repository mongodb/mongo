
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/config_server_op_observer.h"

#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/s/cluster_identity_loader.h"

namespace mongo {

ConfigServerOpObserver::ConfigServerOpObserver() = default;

ConfigServerOpObserver::~ConfigServerOpObserver() = default;

void ConfigServerOpObserver::onDelete(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      OptionalCollectionUUID uuid,
                                      StmtId stmtId,
                                      bool fromMigrate,
                                      const boost::optional<BSONObj>& deletedDoc) {
    if (nss == VersionType::ConfigNS) {
        if (!repl::ReplicationCoordinator::get(opCtx)->getMemberState().rollback()) {
            uasserted(40302, "cannot delete config.version document while in --configsvr mode");
        } else {
            // TODO (SERVER-34165): this is only used for rollback via refetch and can be removed
            // with it.
            // Throw out any cached information related to the cluster ID.
            ShardingCatalogManager::get(opCtx)->discardCachedConfigDatabaseInitializationState();
            ClusterIdentityLoader::get(opCtx)->discardCachedClusterId();
        }
    }
}

repl::OpTime ConfigServerOpObserver::onDropCollection(OperationContext* opCtx,
                                                      const NamespaceString& collectionName,
                                                      OptionalCollectionUUID uuid,
                                                      std::uint64_t numRecords,
                                                      const CollectionDropType dropType) {
    if (collectionName == VersionType::ConfigNS) {
        if (!repl::ReplicationCoordinator::get(opCtx)->getMemberState().rollback()) {
            uasserted(40303, "cannot drop config.version document while in --configsvr mode");
        } else {
            // TODO (SERVER-34165): this is only used for rollback via refetch and can be removed
            // with it.
            // Throw out any cached information related to the cluster ID.
            ShardingCatalogManager::get(opCtx)->discardCachedConfigDatabaseInitializationState();
            ClusterIdentityLoader::get(opCtx)->discardCachedClusterId();
        }
    }

    return {};
}

void ConfigServerOpObserver::onReplicationRollback(OperationContext* opCtx,
                                                   const RollbackObserverInfo& rbInfo) {
    if (rbInfo.configServerConfigVersionRolledBack) {
        // Throw out any cached information related to the cluster ID.
        ShardingCatalogManager::get(opCtx)->discardCachedConfigDatabaseInitializationState();
        ClusterIdentityLoader::get(opCtx)->discardCachedClusterId();
    }
}

}  // namespace mongo
