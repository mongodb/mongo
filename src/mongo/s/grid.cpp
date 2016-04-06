/**
 *    Copyright (C) 2010-2015 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/s/grid.h"

#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"

namespace mongo {

// Global grid instance
Grid grid;

Grid::Grid() : _allowLocalShard(true) {}

void Grid::init(std::unique_ptr<CatalogManager> catalogManager,
                std::unique_ptr<CatalogCache> catalogCache,
                std::unique_ptr<ShardRegistry> shardRegistry,
                std::unique_ptr<ClusterCursorManager> cursorManager) {
    invariant(!_catalogManager);
    invariant(!_catalogCache);
    invariant(!_shardRegistry);
    invariant(!_cursorManager);

    _catalogManager = std::move(catalogManager);
    _catalogCache = std::move(catalogCache);
    _shardRegistry = std::move(shardRegistry);
    _cursorManager = std::move(cursorManager);
}

bool Grid::allowLocalHost() const {
    return _allowLocalShard;
}

void Grid::setAllowLocalHost(bool allow) {
    _allowLocalShard = allow;
}

void Grid::advanceConfigOpTime(repl::OpTime opTime) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_configOpTime < opTime) {
        _configOpTime = opTime;
    }
}

// Note: shardRegistry->shutdown() must be called before this method is called.
void Grid::clearForUnitTests() {
    _catalogManager.reset();
    _catalogCache.reset();
    _shardRegistry.reset();
    _cursorManager.reset();
    _configOpTime = repl::OpTime();
}

}  // namespace mongo
