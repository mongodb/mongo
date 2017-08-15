/**
*    Copyright (C) 2014 MongoDB Inc.
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

#pragma once

#include <string>

#include "mongo/bson/bsonobj.h"
#include "mongo/stdx/functional.h"

namespace mongo {
class CollectionCatalogEntry;
class DatabaseCatalogEntry;
class OperationContext;
class Status;
class StorageEngine;
class StringData;

typedef std::pair<std::vector<std::string>, std::vector<BSONObj>> IndexNameObjs;

/**
 * Returns a pair of parallel vectors. The first item is the index name. The second is the
 * `BSONObj` "index spec" with an index name matching the `filter`.
 *
 * @param filter is a predicate that is passed in an index name, returning true if the index
 *               should be included in the result.
 */
StatusWith<IndexNameObjs> getIndexNameObjs(OperationContext* opCtx,
                                           DatabaseCatalogEntry* dbce,
                                           CollectionCatalogEntry* cce,
                                           stdx::function<bool(const std::string&)> filter =
                                               [](const std::string& indexName) { return true; });

/**
 * Selectively rebuild some indexes on a collection. Indexes will be built in parallel with a
 * `MultiIndexBlock`. One example usage is when a `dropIndex` command is rolled back. The dropped
 * index must be remade.
 *
 * @param indexNameObjs is expected to be the result of a call to `getIndexNameObjs`.
 */
Status rebuildIndexesOnCollection(OperationContext* opCtx,
                                  DatabaseCatalogEntry* dbce,
                                  CollectionCatalogEntry* cce,
                                  const IndexNameObjs& indexNameObjs);

/**
 * Repairs a database using a storage engine-specific, best-effort process.
 * Some data may be lost or modified in the process but the output will
 * be structurally valid on successful return.
 */
Status repairDatabase(OperationContext* opCtx,
                      StorageEngine* engine,
                      const std::string& dbName,
                      bool preserveClonedFilesOnFailure = false,
                      bool backupOriginalFiles = false);
}
