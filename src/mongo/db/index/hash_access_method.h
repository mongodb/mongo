// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/record_id.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/util/modules.h"
#include "mongo/util/shared_buffer_fragment.h"

#include <memory>
#include <string>

#include <boost/optional/optional.hpp>

namespace mongo {

class CollatorInterface;

/**
 * This is the access method for "hashed" indices.
 */
class [[MONGO_MOD_PUBLIC]] HashAccessMethod : public SortedDataIndexAccessMethod {
public:
    HashAccessMethod(IndexCatalogEntry* btreeState, std::unique_ptr<SortedDataInterface> btree);

private:
    void validateDocument(const CollectionPtr& collection,
                          const BSONObj& obj,
                          const BSONObj& keyPattern) const override;

    /**
     * Fills 'keys' with the keys that should be generated for 'obj' on this index.
     *
     * This function ignores the 'multikeyPaths' and 'multikeyMetadataKeys' pointers because hashed
     * indexes don't support tracking path-level multikey information.
     */
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

    BSONObj _keyPattern;

    // _hashVersion defaults to zero.
    int _hashVersion;

    // Null if this index orders strings according to the simple binary compare. If non-null,
    // represents the collator used to generate index keys for indexed strings.
    const CollatorInterface* _collator;
};

}  // namespace mongo
