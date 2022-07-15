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

#include "mongo/db/index/2d_access_method.h"

#include <string>
#include <vector>

#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/index/2d_common.h"
#include "mongo/db/index/expression_keys_private.h"
#include "mongo/db/index/expression_params.h"
#include "mongo/db/index_names.h"
#include "mongo/db/jsobj.h"

namespace mongo {

TwoDAccessMethod::TwoDAccessMethod(IndexCatalogEntry* btreeState,
                                   std::unique_ptr<SortedDataInterface> btree)
    : SortedDataIndexAccessMethod(btreeState, std::move(btree)) {
    const IndexDescriptor* descriptor = btreeState->descriptor();

    ExpressionParams::parseTwoDParams(descriptor->infoObj(), &_params);
}

void TwoDAccessMethod::validateDocument(const CollectionPtr& collection,
                                        const BSONObj& obj,
                                        const BSONObj& keyPattern) const {
    ExpressionKeysPrivate::validateDocumentCommon(collection, obj, keyPattern);
}

/** Finds the key objects to put in an index */
void TwoDAccessMethod::doGetKeys(OperationContext* opCtx,
                                 const CollectionPtr& collection,
                                 SharedBufferFragmentBuilder& pooledBufferBuilder,
                                 const BSONObj& obj,
                                 GetKeysContext context,
                                 KeyStringSet* keys,
                                 KeyStringSet* multikeyMetadataKeys,
                                 MultikeyPaths* multikeyPaths,
                                 const boost::optional<RecordId>& id) const {
    ExpressionKeysPrivate::get2DKeys(pooledBufferBuilder,
                                     obj,
                                     _params,
                                     keys,
                                     getSortedDataInterface()->getKeyStringVersion(),
                                     getSortedDataInterface()->getOrdering(),
                                     id);
}

}  // namespace mongo
