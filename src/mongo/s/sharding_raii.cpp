/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/s/sharding_raii.h"

#include "mongo/base/status_with.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/grid.h"

namespace mongo {

using std::shared_ptr;

ScopedShardDatabase::ScopedShardDatabase(std::shared_ptr<DBConfig> db) : _db(db) {
    invariant(_db);
}

ScopedShardDatabase::~ScopedShardDatabase() = default;

StatusWith<ScopedShardDatabase> ScopedShardDatabase::getExisting(OperationContext* txn,
                                                                 StringData dbName) {
    auto dbStatus = Grid::get(txn)->catalogCache()->getDatabase(txn, dbName.toString());
    if (!dbStatus.isOK()) {
        return {dbStatus.getStatus().code(),
                str::stream() << "Database " << dbName << " was not found due to "
                              << dbStatus.getStatus().toString()};
    }

    return {ScopedShardDatabase(std::move(dbStatus.getValue()))};
}

StatusWith<ScopedShardDatabase> ScopedShardDatabase::getOrCreate(OperationContext* txn,
                                                                 StringData dbName) {
    auto dbStatus = getExisting(txn, dbName);
    if (dbStatus.isOK()) {
        return dbStatus;
    }

    if (dbStatus == ErrorCodes::NamespaceNotFound) {
        auto statusCreateDb =
            Grid::get(txn)->catalogClient(txn)->createDatabase(txn, dbName.toString());
        if (statusCreateDb.isOK() || statusCreateDb == ErrorCodes::NamespaceExists) {
            return getExisting(txn, dbName);
        }

        return statusCreateDb;
    }

    return dbStatus.getStatus();
}

ScopedChunkManager::ScopedChunkManager(ScopedShardDatabase db, std::shared_ptr<ChunkManager> cm)
    : _db(std::move(db)), _cm(std::move(cm)) {}

ScopedChunkManager::~ScopedChunkManager() = default;

StatusWith<ScopedChunkManager> ScopedChunkManager::getExisting(OperationContext* txn,
                                                               const NamespaceString& nss) {
    auto scopedDbStatus = ScopedShardDatabase::getExisting(txn, nss.db());
    if (!scopedDbStatus.isOK()) {
        return scopedDbStatus.getStatus();
    }

    auto scopedDb = std::move(scopedDbStatus.getValue());

    shared_ptr<ChunkManager> cm = scopedDb.db()->getChunkManagerIfExists(txn, nss.ns(), true);
    if (!cm) {
        return {ErrorCodes::NamespaceNotSharded,
                str::stream() << "Collection " << nss.ns() << " does not exist or is not sharded."};
    }

    if (cm->getChunkMap().empty()) {
        return {ErrorCodes::NamespaceNotSharded,
                str::stream() << "Collection " << nss.ns() << " does not have any chunks."};
    }

    return {ScopedChunkManager(std::move(scopedDb), std::move(cm))};
}

}  // namespace mongo
