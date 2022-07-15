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

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/db/catalog/validate_state.h"
#include "mongo/db/operation_context.h"

namespace mongo {
namespace index_repair {

/**
 * Deletes the record containing a duplicate key and inserts it into a local lost and found
 * collection titled "local.lost_and_found.<original collection UUID>". Returns the size of the
 * record removed.
 */
StatusWith<int> moveRecordToLostAndFound(OperationContext* opCtx,
                                         const NamespaceString& ns,
                                         const NamespaceString& lostAndFoundNss,
                                         const RecordId& dupRecord);

/**
 * If repair mode is enabled, tries the inserting missingIndexEntry into indexes. If the
 * missingIndexEntry is a duplicate on a unique index, removes the duplicate document and keeps it
 * in a local lost and found collection.
 */
int repairMissingIndexEntry(OperationContext* opCtx,
                            std::shared_ptr<const IndexCatalogEntry>& index,
                            const KeyString::Value& ks,
                            const KeyFormat& keyFormat,
                            const NamespaceString& nss,
                            const CollectionPtr& coll,
                            ValidateResults* results);

}  // namespace index_repair
}  // namespace mongo
