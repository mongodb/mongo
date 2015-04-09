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

#include <boost/make_shared.hpp>

#include "mongo/base/status_with.h"
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/config.h"
#include "mongo/s/type_database.h"

namespace mongo {

    using boost::shared_ptr;
    using std::string;


    CatalogCache::CatalogCache(CatalogManager* catalogManager)
            : _catalogManager(catalogManager) {

        invariant(_catalogManager);
    }

    StatusWith<shared_ptr<DBConfig>> CatalogCache::getDatabase(const string& dbName) {
        boost::lock_guard<boost::mutex> guard(_mutex);

        ShardedDatabasesMap::iterator it = _databases.find(dbName);
        if (it != _databases.end()) {
            return it->second;
        }

        // Need to load from the store
        StatusWith<DatabaseType> status = _catalogManager->getDatabase(dbName);
        if (!status.isOK()) {
            return status.getStatus();
        }

        shared_ptr<DBConfig> db = boost::make_shared<DBConfig>(dbName, status.getValue());
        db->load();

        invariant(_databases.insert(std::make_pair(dbName, db)).second);

        return db;
    }

    void CatalogCache::invalidate(const string& dbName) {
        boost::lock_guard<boost::mutex> guard(_mutex);

        ShardedDatabasesMap::iterator it = _databases.find(dbName);
        if (it != _databases.end()) {
            _databases.erase(it);
        }
    }

    void CatalogCache::invalidateAll() {
        boost::lock_guard<boost::mutex> guard(_mutex);

        _databases.clear();
    }

} // namespace mongo
