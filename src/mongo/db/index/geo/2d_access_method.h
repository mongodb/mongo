// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/index/geo/2d_common.h"
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

class IndexCatalogEntry;
class IndexDescriptor;
struct TwoDIndexingParams;

// Public: instantiated in index_access_method.cpp (index_builds module)
class [[MONGO_MOD_PUBLIC]] TwoDAccessMethod : public SortedDataIndexAccessMethod {
public:
    TwoDAccessMethod(IndexCatalogEntry* btreeState, std::unique_ptr<SortedDataInterface> btree);

private:
    TwoDIndexingParams& getParams() {
        return _params;
    }

    void validateDocument(const CollectionPtr& collection,
                          const BSONObj& obj,
                          const BSONObj& keyPattern) const override;

    /**
     * Fills 'keys' with the keys that should be generated for 'obj' on this index.
     *
     * This function ignores the 'multikeyPaths' and 'multikeyMetadataKeys' pointers because 2d
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

    TwoDIndexingParams _params;
};

}  // namespace mongo
