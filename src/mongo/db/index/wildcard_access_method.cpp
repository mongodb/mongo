// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index/wildcard_access_method.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/index_names.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"

#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {

WildcardAccessMethod::WildcardAccessMethod(IndexCatalogEntry* wildcardState,
                                           std::unique_ptr<SortedDataInterface> btree)
    : SortedDataIndexAccessMethod(wildcardState, std::move(btree)),
      _keyGen(wildcardState->descriptor()->keyPattern(),
              wildcardState->descriptor()->pathProjection(),
              wildcardState->getCollator(),
              getSortedDataInterface()->getKeyStringVersion(),
              getSortedDataInterface()->getOrdering(),
              getSortedDataInterface()->rsKeyFormat()) {}

bool WildcardAccessMethod::shouldMarkIndexAsMultikey(size_t numberOfKeys,
                                                     const KeyStringSet& multikeyMetadataKeys,
                                                     const MultikeyPaths& multikeyPaths) const {
    return !multikeyMetadataKeys.empty();
}

void WildcardAccessMethod::doGetKeys(OperationContext* opCtx,
                                     const CollectionPtr& collection,
                                     const IndexCatalogEntry* entry,
                                     SharedBufferFragmentBuilder& pooledBufferBuilder,
                                     const BSONObj& obj,
                                     GetKeysContext context,
                                     KeyStringSet* keys,
                                     KeyStringSet* multikeyMetadataKeys,
                                     MultikeyPaths* multikeyPaths,
                                     const boost::optional<RecordId>& id) const {
    _keyGen.generateKeys(pooledBufferBuilder, obj, keys, multikeyMetadataKeys, id);
}

Ordering WildcardAccessMethod::makeOrdering(const BSONObj& pattern) {
    BSONObjBuilder newPattern;
    for (auto elem : pattern) {
        const auto fieldName = elem.fieldNameStringData();
        if (WildcardNames::isWildcardFieldName(fieldName)) {
            newPattern.append("$_path", 1);  // "$_path" should always be in ascending order.
        }
        newPattern.append(elem);
    }

    return Ordering::make(newPattern.obj());
}
}  // namespace mongo
