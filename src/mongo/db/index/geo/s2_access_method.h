// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/index/geo/s2_common.h"
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
#include <set>

#include <boost/optional/optional.hpp>

namespace mongo {

// Public: instantiated in index_access_method.cpp (index_builds module) and fixSpec() called from
// index_catalog_impl.cpp (catalog_and_routing.shard_role module)
class [[MONGO_MOD_PUBLIC]] S2AccessMethod : public SortedDataIndexAccessMethod {
public:
    S2AccessMethod(IndexCatalogEntry* btreeState, std::unique_ptr<SortedDataInterface> btree)
        : S2AccessMethod(btreeState, std::move(btree), IndexNames::GEO_2DSPHERE) {}

    /**
     * Helper for 'fixSpec' which validates the index and returns a copy tweaked to conform to the
     * expected format. If allowedVersions is specified, the index version (or default version if
     * not specified) must be in the allowed set.
     *
     * Returns a non-OK status if 'specObj' is invalid.
     */
    static StatusWith<BSONObj> _fixSpecHelper(
        const BSONObj& specObj, boost::optional<std::set<long long>> allowedVersions = boost::none);

    /**
     * Takes an index spec object for this index and returns a copy tweaked to conform to the
     * expected format. When an index build is initiated, this function is called on the spec
     * object the user provides, and the return value of this function is the final spec object
     * that gets saved in the index catalog.
     *
     * Returns a non-OK status if 'specObj' is invalid.
     */
    static StatusWith<BSONObj> fixSpec(const BSONObj& specObj);

    /**
     * Public API for checking if an S2 index is version 3.
     */
    static bool isVersion3(const BSONObj& indexSpec);

    /**
     * For S2 indexes, this only returns true for version 3 indexes that may need to be checked for
     * version 4 upgrade scenarios.
     */
    bool shouldCheckMissingIndexEntryAlternative(OperationContext* opCtx,
                                                 const IndexCatalogEntry& entry) const override;

    /**
     * Checks if a missing index entry is actually present when using version 4 key generation,
     * indicating the index needs to be upgraded from version 3 to version 4.
     */
    boost::optional<std::pair<std::string, std::string>> checkMissingIndexEntryAlternative(
        OperationContext* opCtx,
        const IndexCatalogEntry& entry,
        const key_string::Value& missingKey,
        const RecordId& recordId,
        const BSONObj& document) const override;

protected:
    S2AccessMethod(IndexCatalogEntry* btreeState,
                   std::unique_ptr<SortedDataInterface> btree,
                   const std::string& indexName);

private:
    void validateDocument(const CollectionPtr& collection,
                          const BSONObj& obj,
                          const BSONObj& keyPattern) const final;

    /**
     * Fills 'keys' with the keys that should be generated for 'obj' on this index.
     *
     * This function ignores the 'multikeyMetadataKeys' pointer since those are only used for
     * wildcard indexes.
     *
     * If the 'multikeyPaths' pointer is non-null, then it must point to an empty vector. This
     * function resizes 'multikeyPaths' to have the same number of elements as the index key pattern
     * and fills each element with the prefixes of the indexed field that would cause this index to
     * be multikey as a result of inserting 'keys'.
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

    S2IndexingParams _params;
};

}  // namespace mongo
