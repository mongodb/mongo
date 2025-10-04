/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/record_id.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace mongo {
class Collection;
class NamespaceString;
class OperationContext;

typedef std::pair<std::vector<std::string>, std::vector<BSONObj>> IndexNameObjs;

/**
 * Returns a pair of parallel vectors. The first item is the index name. The second is the
 * `BSONObj` "index spec" with an index name matching the `filter`.
 *
 * @param filter is a predicate that is passed in an index name, returning true if the index
 *               should be included in the result.
 */
StatusWith<IndexNameObjs> getIndexNameObjs(
    const Collection* collection,
    std::function<bool(const std::string&)> filter = [](const std::string& indexName) {
        return true;
    });

/**
 * Rebuilds the indexes provided by the 'indexSpecs' on the given collection.
 * One example usage is when a 'dropIndex' command is rolled back. The dropped index must be remade.
 * When 'repair' is set to kYes, this function will delete corrupt records when found, rather than
 * crashing.
 */
enum class RepairData { kYes, kNo };
Status rebuildIndexesOnCollection(OperationContext* opCtx,
                                  CollectionWriter& collWriter,
                                  const std::vector<BSONObj>& indexSpecs,
                                  RepairData repair);

}  // namespace mongo
