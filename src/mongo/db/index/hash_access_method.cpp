// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index/hash_access_method.h"

#include "mongo/db/index/expression_keys_private.h"
#include "mongo/db/index/expression_params.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/util/assert_util.h"

#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {

HashAccessMethod::HashAccessMethod(IndexCatalogEntry* btreeState,
                                   std::unique_ptr<SortedDataInterface> btree)
    : SortedDataIndexAccessMethod(btreeState, std::move(btree)) {
    const IndexDescriptor* descriptor = btreeState->descriptor();

    uassert(16764,
            "Currently hashed indexes cannot guarantee uniqueness. Use a regular index.",
            !descriptor->unique());
    ExpressionParams::parseHashParams(descriptor->infoObj(), &_hashVersion, &_keyPattern);

    _collator = btreeState->getCollator();
}

void HashAccessMethod::validateDocument(const CollectionPtr& collection,
                                        const BSONObj& obj,
                                        const BSONObj& keyPattern) const {
    ExpressionKeysPrivate::validateDocumentCommon(collection, obj, keyPattern);
}

void HashAccessMethod::doGetKeys(OperationContext* opCtx,
                                 const CollectionPtr& collection,
                                 const IndexCatalogEntry* entry,
                                 SharedBufferFragmentBuilder& pooledBufferBuilder,
                                 const BSONObj& obj,
                                 GetKeysContext context,
                                 KeyStringSet* keys,
                                 KeyStringSet* multikeyMetadataKeys,
                                 MultikeyPaths* multikeyPaths,
                                 const boost::optional<RecordId>& id) const {
    ExpressionKeysPrivate::getHashKeys(pooledBufferBuilder,
                                       obj,
                                       _keyPattern,
                                       _hashVersion,
                                       entry->descriptor()->isSetSparseByUser(),
                                       _collator,
                                       keys,
                                       getSortedDataInterface()->getKeyStringVersion(),
                                       getSortedDataInterface()->getOrdering(),
                                       (context == GetKeysContext::kRemovingKeys),
                                       id);
}

}  // namespace mongo
