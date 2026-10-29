// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index/geo/2d_access_method.h"

#include "mongo/db/index/expression_keys_private.h"
#include "mongo/db/index/geo/2d_key_generator.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"

#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {
TwoDAccessMethod::TwoDAccessMethod(IndexCatalogEntry* btreeState,
                                   std::unique_ptr<SortedDataInterface> btree)
    : SortedDataIndexAccessMethod(btreeState, std::move(btree)) {
    const IndexDescriptor* descriptor = btreeState->descriptor();

    index2d::parse2dParams(descriptor->infoObj(), &_params);
}

void TwoDAccessMethod::validateDocument(const CollectionPtr& collection,
                                        const BSONObj& obj,
                                        const BSONObj& keyPattern) const {
    ExpressionKeysPrivate::validateDocumentCommon(collection, obj, keyPattern);
}

/** Finds the key objects to put in an index */
void TwoDAccessMethod::doGetKeys(OperationContext* opCtx,
                                 const CollectionPtr& collection,
                                 const IndexCatalogEntry* entry,
                                 SharedBufferFragmentBuilder& pooledBufferBuilder,
                                 const BSONObj& obj,
                                 GetKeysContext context,
                                 KeyStringSet* keys,
                                 KeyStringSet* multikeyMetadataKeys,
                                 MultikeyPaths* multikeyPaths,
                                 const boost::optional<RecordId>& id) const {
    index2d::get2DKeys(pooledBufferBuilder,
                       obj,
                       _params,
                       keys,
                       getSortedDataInterface()->getKeyStringVersion(),
                       getSortedDataInterface()->getOrdering(),
                       id);
}

}  // namespace mongo
