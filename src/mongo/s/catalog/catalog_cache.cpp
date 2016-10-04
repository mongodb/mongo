/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/s/catalog/catalog_cache.h"


#include "mongo/base/status_with.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/config.h"
#include "mongo/s/grid.h"

namespace mongo {

using std::shared_ptr;
using std::string;


CatalogCache::CatalogCache() {}

StatusWith<shared_ptr<DBConfig>> CatalogCache::getDatabase(OperationContext* txn,
                                                           const string& dbName) {
    stdx::lock_guard<stdx::mutex> guard(_mutex);

    ShardedDatabasesMap::iterator it = _databases.find(dbName);
    if (it != _databases.end()) {
        return it->second;
    }

    // Need to load from the store
    auto status = grid.catalogClient(txn)->getDatabase(txn, dbName);
    if (!status.isOK()) {
        return status.getStatus();
    }

    const auto dbOpTimePair = status.getValue();
    shared_ptr<DBConfig> db =
        std::make_shared<DBConfig>(dbName, dbOpTimePair.value, dbOpTimePair.opTime);
    db->load(txn);

    invariant(_databases.insert(std::make_pair(dbName, db)).second);

    return db;
}

void CatalogCache::invalidate(const string& dbName) {
    stdx::lock_guard<stdx::mutex> guard(_mutex);

    ShardedDatabasesMap::iterator it = _databases.find(dbName);
    if (it != _databases.end()) {
        _databases.erase(it);
    }
}

void CatalogCache::invalidateAll() {
    stdx::lock_guard<stdx::mutex> guard(_mutex);

    _databases.clear();
}

}  // namespace mongo
