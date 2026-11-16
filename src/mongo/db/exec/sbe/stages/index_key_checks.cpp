// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/stages/index_key_checks.h"

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/index/preallocated_container_pool.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/shared_buffer_fragment.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::sbe {

void checkIndexKeyCorruption(OperationContext* opCtx,
                             value::SlotAccessor* indexKeyAccessor,
                             value::SlotAccessor* indexKeyPatternAccessor,
                             const RecordId& rid,
                             const NamespaceString& nss) {
    // Having a recordId but no record is only an issue when we are not ignoring prepare conflicts.
    if (!ignoresPrepareConflicts(
            shard_role_details::getRecoveryUnit(opCtx)->getPrepareConflictBehavior())) {
        tassert(5113703, "Should have index key accessor", indexKeyAccessor);
        tassert(5113704, "Should have key pattern accessor", indexKeyPatternAccessor);

        auto [ksTag, ksVal] = indexKeyAccessor->getViewOfValue();
        auto [kpTag, kpVal] = indexKeyPatternAccessor->getViewOfValue();

        const auto msgKsTag = ksTag;
        tassert(5113706,
                str::stream() << "KeyString is of wrong type: " << msgKsTag,
                ksTag == value::TypeTags::keyString);

        const auto msgKpTag = kpTag;
        tassert(5113707,
                str::stream() << "Index key pattern is of wrong type: " << msgKpTag,
                kpTag == value::TypeTags::bsonObject);

        auto keyString = value::getKeyString(ksVal);
        tassert(5113708, "KeyString does not exist", keyString);

        BSONObj bsonKeyPattern(value::bitcastTo<const char*>(kpVal));
        auto bsonKeyString = key_string::toBson(keyString->getKeyStringView(),
                                                Ordering::make(bsonKeyPattern),
                                                keyString->getTypeBitsView(),
                                                keyString->getVersion());
        auto hydratedKey = IndexKeyEntry::rehydrateKey(bsonKeyPattern, bsonKeyString);

        LOGV2_ERROR_OPTIONS(
            5113709,
            {logv2::UserAssertAfterLog(ErrorCodes::DataCorruptionDetected)},
            "Erroneous index key found with reference to non-existent record id. Consider "
            "dropping and then re-creating the index and then running the validate command "
            "on the collection.",
            logAttrs(nss),
            "recordId"_attr = rid,
            "indexKeyData"_attr = hydratedKey);
    }
}

bool isIndexKeyConsistent(OperationContext* opCtx,
                          StringMap<const IndexCatalogEntry*>& entryMap,
                          value::SlotAccessor* indexIdentAccessor,
                          value::SlotAccessor* indexKeyAccessor,
                          const CollectionPtr& collection,
                          const Record& nextRecord) {
    tassert(5290707, "Should have index key accessor", indexKeyAccessor);
    tassert(5290714, "Should have index ident accessor", indexIdentAccessor);

    auto [identTag, identVal] = indexIdentAccessor->getViewOfValue();
    auto [ksTag, ksVal] = indexKeyAccessor->getViewOfValue();

    const auto msgIdentTag = identTag;
    tassert(5290708,
            str::stream() << "Index name is of wrong type: " << msgIdentTag,
            value::isString(identTag));

    const auto msgKsTag = ksTag;
    tassert(5290710,
            str::stream() << "KeyString is of wrong type: " << msgKsTag,
            ksTag == value::TypeTags::keyString);

    auto keyString = value::getKeyString(ksVal);
    auto indexIdent = value::getStringView(identTag, identVal);
    tassert(5290712, "KeyString does not exist", keyString);

    auto it = entryMap.find(indexIdent);

    // If 'entryMap' doesn't contain an entry for 'indexIdent', create one.
    if (it == entryMap.end()) {
        auto indexCatalog = collection->getIndexCatalog();
        auto entry = indexCatalog->findIndexByIdent(opCtx, indexIdent);
        auto indexDesc = entry ? entry->descriptor() : nullptr;

        // Throw an error if we can't get the IndexDescriptor or the IndexCatalogEntry
        // (or if the index is dropped).
        uassert(ErrorCodes::QueryPlanKilled,
                str::stream() << "query plan killed :: index dropped: " << indexIdent,
                indexDesc && entry);

        auto [newIt, _] = entryMap.emplace(indexIdent, entry);

        it = newIt;
    }

    auto entry = it->second;
    auto iam = entry->accessMethod()->asSortedData();
    tassert(5290709,
            str::stream() << "Expected to find SortedDataIndexAccessMethod for index: "
                          << indexIdent,
            iam);

    auto& containerPool = PreallocatedContainerPool::get(opCtx);
    auto keys = containerPool.keys();
    SharedBufferFragmentBuilder pooledBuilder(key_string::HeapBuilder::kHeapAllocatorDefaultBytes);

    // There's no need to compute the prefixes of the indexed fields that cause the
    // index to be multikey when ensuring the keyData is still valid.
    KeyStringSet* multikeyMetadataKeys = nullptr;
    MultikeyPaths* multikeyPaths = nullptr;

    iam->getKeys(opCtx,
                 collection,
                 entry,
                 pooledBuilder,
                 nextRecord.data.toBson(),
                 InsertDeleteOptions::ConstraintEnforcementMode::kEnforceConstraints,
                 SortedDataIndexAccessMethod::GetKeysContext::kValidatingKeys,
                 keys.get(),
                 multikeyMetadataKeys,
                 multikeyPaths,
                 nextRecord.id);

    return keys->count(keyString->getValueCopy());
}

}  // namespace mongo::sbe
