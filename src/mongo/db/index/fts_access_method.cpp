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

#include "mongo/db/index/fts_access_method.h"

#include "mongo/db/fts/fts_index_format.h"
#include "mongo/db/index/expression_keys_private.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/recovery_unit.h"

#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {

FTSAccessMethod::FTSAccessMethod(IndexCatalogEntry* btreeState,
                                 std::unique_ptr<SortedDataInterface> btree)
    : SortedDataIndexAccessMethod(btreeState, std::move(btree)),
      _ftsSpec(btreeState->descriptor()->infoObj()) {}

void FTSAccessMethod::doGetKeys(OperationContext* opCtx,
                                const CollectionPtr& collection,
                                const IndexCatalogEntry* entry,
                                SharedBufferFragmentBuilder& pooledBufferBuilder,
                                const BSONObj& obj,
                                GetKeysContext context,
                                KeyStringSet* keys,
                                KeyStringSet* multikeyMetadataKeys,
                                MultikeyPaths* multikeyPaths,
                                const boost::optional<RecordId>& id) const {
    ExpressionKeysPrivate::getFTSKeys(pooledBufferBuilder,
                                      obj,
                                      _ftsSpec,
                                      keys,
                                      getSortedDataInterface()->getKeyStringVersion(),
                                      getSortedDataInterface()->getOrdering(),
                                      id);
}

bool FTSAccessMethod::shouldCheckMissingIndexEntryAlternative(
    OperationContext* opCtx, const IndexCatalogEntry& entry) const {
    auto versionElt = entry.descriptor()->infoObj()["textIndexVersion"];
    return versionElt.isNumber() && versionElt.numberInt() == fts::TEXT_INDEX_VERSION_3;
}

boost::optional<std::pair<std::string, std::string>>
FTSAccessMethod::checkMissingIndexEntryAlternative(OperationContext* opCtx,
                                                   const IndexCatalogEntry& entry,
                                                   const key_string::Value& missingKey,
                                                   const RecordId& recordId,
                                                   const BSONObj& document) const {
    auto versionElt = entry.descriptor()->infoObj()["textIndexVersion"];
    if (!versionElt.isNumber() || versionElt.numberInt() != fts::TEXT_INDEX_VERSION_3) {
        return boost::none;
    }

    // Generate keys using the legacy pre-SERVER-76875 behavior.
    // This will include keys for fields with embedded dots.
    KeyStringSet legacyKeys =
        generateKeysLegacyDottedPath_forValidationOnly(opCtx, &entry, document, recordId);

    // Check if any legacy keys exist in the index.
    if (!legacyKeys.empty()) {
        auto sortedDataInterface = getSortedDataInterface();
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
        auto cursor = sortedDataInterface->newCursor(opCtx, ru);

        // Look for any of the legacy keys in the actual index.
        bool foundMatchingKey =
            std::any_of(legacyKeys.begin(), legacyKeys.end(), [&](const auto& keyVersionX) {
                // seekForKeyString returns the KeyStringEntry if the key exists.
                auto ksEntry = cursor->seekForKeyString(ru, keyVersionX.getView());
                // Verify both the key exists AND it points to this record.
                return ksEntry && ksEntry->loc == recordId;
            });

        if (foundMatchingKey) {
            // We found a legacy key in the index.
            const std::string indexName = entry.descriptor()->indexName();
            std::string errorMsg = "Index '" + indexName +
                "' was created with a legacy version of text index key generation that included "
                "fields with embedded dots. This index needs to be rebuilt, please drop and "
                "recreate it.";
            std::string warningMsg = "Index '" + indexName +
                "' needs to be rebuilt due to SERVER-76875 "
                "(fields with embedded dots in text indexes).";
            return std::make_pair(errorMsg, warningMsg);
        }
    }

    // No alternative explanation found - this is a genuine missing index entry.
    return boost::none;
}

KeyStringSet FTSAccessMethod::generateKeysLegacyDottedPath_forValidationOnly(
    OperationContext* opCtx,
    const IndexCatalogEntry* entry,
    const BSONObj& obj,
    const RecordId& id) const {
    KeyStringSet keys;
    SharedBufferFragmentBuilder pooledBufferBuilder{
        key_string::HeapBuilder::kHeapAllocatorDefaultBytes};

    // Use the legacy FTS key generation function.
    fts::FTSIndexFormat::getKeysLegacy_forValidationOnly(
        pooledBufferBuilder,
        _ftsSpec,
        obj,
        &keys,
        getSortedDataInterface()->getKeyStringVersion(),
        getSortedDataInterface()->getOrdering(),
        id);

    return keys;
}

}  // namespace mongo
