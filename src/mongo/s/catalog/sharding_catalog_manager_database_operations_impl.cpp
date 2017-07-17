/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/s/catalog/sharding_catalog_manager.h"

#include "mongo/db/namespace_string.h"
#include "mongo/s/catalog/sharding_catalog_client_impl.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace mongo {

Status ShardingCatalogManager::enableSharding(OperationContext* opCtx, const std::string& dbName) {
    invariant(nsIsDbOnly(dbName));

    if (dbName == NamespaceString::kConfigDb || dbName == NamespaceString::kAdminDb) {
        return {
            ErrorCodes::IllegalOperation,
            str::stream() << "Enabling sharding on system configuration databases is not allowed"};
    }

    // Lock the database globally to prevent conflicts with simultaneous database
    // creation/modification.
    auto scopedDistLock = Grid::get(opCtx)->catalogClient()->getDistLockManager()->lock(
        opCtx, dbName, "enableSharding", DistLockManager::kDefaultLockTimeout);
    if (!scopedDistLock.isOK()) {
        return scopedDistLock.getStatus();
    }

    // Check for case sensitivity violations
    DatabaseType db;
    Status status = Grid::get(opCtx)->catalogClient()->_checkDbDoesNotExist(opCtx, dbName, &db);
    if (status.isOK()) {
        // Database does not exist, create a new entry
        auto newShardIdStatus = ShardingCatalogClientImpl::_selectShardForNewDatabase(
            opCtx, Grid::get(opCtx)->shardRegistry());
        if (!newShardIdStatus.isOK()) {
            return newShardIdStatus.getStatus();
        }

        const ShardId& newShardId = newShardIdStatus.getValue();

        log() << "Placing [" << dbName << "] on: " << newShardId;

        db.setName(dbName);
        db.setPrimary(newShardId);
        db.setSharded(true);
    } else if (status.code() == ErrorCodes::NamespaceExists) {
        if (db.getSharded()) {
            return Status::OK();
        }

        // Database exists, so just update it
        db.setSharded(true);
    } else {
        return status;
    }

    log() << "Enabling sharding for database [" << dbName << "] in config db";

    return Grid::get(opCtx)->catalogClient()->updateDatabase(opCtx, dbName, db);
}

}  // namespace mongo
