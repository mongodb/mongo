// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index/btree_access_method.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/db/index/expression_keys_private.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"

#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

using std::vector;

// Standard Btree implementation below.
BtreeAccessMethod::BtreeAccessMethod(IndexCatalogEntry* btreeState,
                                     std::unique_ptr<SortedDataInterface> btree)
    : SortedDataIndexAccessMethod(btreeState, std::move(btree)) {
    // The key generation wants these values.
    vector<const char*> fieldNames;
    vector<BSONElement> fixed;

    BSONObjIterator it(btreeState->descriptor()->keyPattern());
    while (it.more()) {
        BSONElement elt = it.next();
        fieldNames.push_back(elt.fieldName());
        fixed.push_back(BSONElement());
    }

    _keyGenerator =
        std::make_unique<BtreeKeyGenerator>(fieldNames,
                                            fixed,
                                            btreeState->descriptor()->isSetSparseByUser(),
                                            getSortedDataInterface()->getKeyStringVersion(),
                                            getSortedDataInterface()->getOrdering());
}

void BtreeAccessMethod::validateDocument(const CollectionPtr& collection,
                                         const BSONObj& obj,
                                         const BSONObj& keyPattern) const {
    ExpressionKeysPrivate::validateDocumentCommon(collection, obj, keyPattern);
}

void BtreeAccessMethod::doGetKeys(OperationContext* opCtx,
                                  const CollectionPtr& collection,
                                  const IndexCatalogEntry* entry,
                                  SharedBufferFragmentBuilder& pooledBufferBuilder,
                                  const BSONObj& obj,
                                  GetKeysContext context,
                                  KeyStringSet* keys,
                                  KeyStringSet* multikeyMetadataKeys,
                                  MultikeyPaths* multikeyPaths,
                                  const boost::optional<RecordId>& id) const {
    const auto skipMultikey =
        context == GetKeysContext::kValidatingKeys && !entry->isMultikey(opCtx, collection);
    _keyGenerator->getKeys(
        pooledBufferBuilder, obj, skipMultikey, keys, multikeyPaths, entry->getCollator(), id);
}

}  // namespace mongo
