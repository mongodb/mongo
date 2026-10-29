// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once


#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/index/btree_key_generator.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/util/modules.h"
#include "mongo/util/shared_buffer_fragment.h"

#include <memory>

#include <boost/optional/optional.hpp>

namespace mongo {

class IndexDescriptor;

/**
 * The IndexAccessMethod for a Btree index.
 * Any index created with {field: 1} or {field: -1} uses this.
 */
class [[MONGO_MOD_PUBLIC]] BtreeAccessMethod : public SortedDataIndexAccessMethod {
public:
    BtreeAccessMethod(IndexCatalogEntry* btreeState, std::unique_ptr<SortedDataInterface> btree);

private:
    void validateDocument(const CollectionPtr& collection,
                          const BSONObj& obj,
                          const BSONObj& keyPattern) const override;

    void doGetKeys(OperationContext* opCtx,
                   const CollectionPtr& collection,
                   const IndexCatalogEntry* entry,
                   SharedBufferFragmentBuilder& pooledBufferBuilder,
                   const BSONObj& obj,
                   GetKeysContext context,
                   KeyStringSet* keys,
                   KeyStringSet* multikeyMetadataKeys,
                   MultikeyPaths* multikeyPaths,
                   const boost::optional<RecordId>& id) const final;

    // Our keys differ for V0 and V1.
    std::unique_ptr<BtreeKeyGenerator> _keyGenerator;
};

}  // namespace mongo
