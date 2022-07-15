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

#include "mongo/db/index/fts_access_method.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/index/expression_keys_private.h"
#include "mongo/db/index/index_descriptor.h"

namespace mongo {

FTSAccessMethod::FTSAccessMethod(IndexCatalogEntry* btreeState,
                                 std::unique_ptr<SortedDataInterface> btree)
    : SortedDataIndexAccessMethod(btreeState, std::move(btree)),
      _ftsSpec(btreeState->descriptor()->infoObj()) {}

void FTSAccessMethod::doGetKeys(OperationContext* opCtx,
                                const CollectionPtr& collection,
                                SharedBufferFragmentBuilder& pooledBufferBuilder,
                                const BSONObj& obj,
                                GetKeysContext context,
                                KeyStringSet* keys,
                                KeyStringSet* multikeyMetadataKeys,
                                MultikeyPaths* multikeyPaths,
                                const boost::optional<RecordId>& id) const {
    ExpressionKeysPrivate::getFTSKeys(pooledBufferBuilder,
                                      obj,
                                      _ftsSpec,
                                      keys,
                                      getSortedDataInterface()->getKeyStringVersion(),
                                      getSortedDataInterface()->getOrdering(),
                                      id);
}

}  // namespace mongo
