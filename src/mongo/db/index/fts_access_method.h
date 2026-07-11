// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/util/modules.h"
#include "mongo/util/shared_buffer_fragment.h"

#include <memory>

#include <boost/optional/optional.hpp>

namespace mongo {

class [[MONGO_MOD_PUBLIC]] FTSAccessMethod : public SortedDataIndexAccessMethod {
public:
    FTSAccessMethod(IndexCatalogEntry* btreeState, std::unique_ptr<SortedDataInterface> btree);

    const fts::FTSSpec& getSpec() const {
        return _ftsSpec;
    }

    /**
     * For text indexes, returns true for TEXT_INDEX_VERSION_3 indexes to check for key generation
     * differences.
     *
     * This allows us to detect TEXT_INDEX_VERSION_3 indexes that were created before SERVER-76875
     * when fields with embedded dots were included in the index. Such indexes need to be rebuilt.
     */
    bool shouldCheckMissingIndexEntryAlternative(OperationContext* opCtx,
                                                 const IndexCatalogEntry& entry) const override;

    /**
     * Checks if a missing text index entry is actually present when using legacy version 3
     * key generation behavior, indicating the index was created before SERVER-76875 and needs to be
     * rebuilt.
     */
    boost::optional<std::pair<std::string, std::string>> checkMissingIndexEntryAlternative(
        OperationContext* opCtx,
        const IndexCatalogEntry& entry,
        const key_string::Value& missingKey,
        const RecordId& recordId,
        const BSONObj& document) const override;

    /**
     * Helper to generate keys using the legacy behavior before SERVER-76875 for validation.
     * This uses the legacy dotted path extraction that checks for literal field names
     * with dots before traversing nested objects.
     */
    KeyStringSet generateKeysLegacyDottedPath_forValidationOnly(OperationContext* opCtx,
                                                                const IndexCatalogEntry* entry,
                                                                const BSONObj& obj,
                                                                const RecordId& id) const;

private:
    /**
     * Helper to generate keys using the current (post-SERVER-76875) behavior for validation.
     */
    KeyStringSet generateKeysCurrent_forValidationOnly(OperationContext* opCtx,
                                                       const IndexCatalogEntry* entry,
                                                       const BSONObj& obj,
                                                       const RecordId& id) const;

    /**
     * Fills 'keys' with the keys that should be generated for 'obj' on this index.
     *
     * This function ignores the 'multikeyPaths' and 'multikeyMetadataKeys' pointers because text
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

    fts::FTSSpec _ftsSpec;
};

}  // namespace mongo
