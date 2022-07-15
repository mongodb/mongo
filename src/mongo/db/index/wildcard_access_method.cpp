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

#include "mongo/platform/basic.h"

#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/wildcard_access_method.h"

#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/query/index_bounds_builder.h"

namespace mongo {

WildcardAccessMethod::WildcardAccessMethod(IndexCatalogEntry* wildcardState,
                                           std::unique_ptr<SortedDataInterface> btree)
    : SortedDataIndexAccessMethod(wildcardState, std::move(btree)),
      _keyGen(_descriptor->keyPattern(),
              _descriptor->pathProjection(),
              _indexCatalogEntry->getCollator(),
              getSortedDataInterface()->getKeyStringVersion(),
              getSortedDataInterface()->getOrdering(),
              getSortedDataInterface()->rsKeyFormat()) {
    // Normalize the 'wildcardProjection' index option to facilitate its comparison as part of
    // index signature.
    if (!_descriptor->pathProjection().isEmpty()) {
        auto* projExec = getWildcardProjection()->exec();
        wildcardState->descriptor()->_setNormalizedPathProjection(
            projExec->serializeTransformation(boost::none).toBson());
    }
}

bool WildcardAccessMethod::shouldMarkIndexAsMultikey(size_t numberOfKeys,
                                                     const KeyStringSet& multikeyMetadataKeys,
                                                     const MultikeyPaths& multikeyPaths) const {
    return !multikeyMetadataKeys.empty();
}

void WildcardAccessMethod::doGetKeys(OperationContext* opCtx,
                                     const CollectionPtr& collection,
                                     SharedBufferFragmentBuilder& pooledBufferBuilder,
                                     const BSONObj& obj,
                                     GetKeysContext context,
                                     KeyStringSet* keys,
                                     KeyStringSet* multikeyMetadataKeys,
                                     MultikeyPaths* multikeyPaths,
                                     const boost::optional<RecordId>& id) const {
    _keyGen.generateKeys(pooledBufferBuilder, obj, keys, multikeyMetadataKeys, id);
}
}  // namespace mongo
