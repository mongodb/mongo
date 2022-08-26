/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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


#include "mongo/db/catalog/coll_mod_index.h"

#include "mongo/db/catalog/cannot_convert_index_to_unique_info.h"
#include "mongo/db/catalog/throttle_cursor.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/ttl_collection_cache.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/shared_buffer_fragment.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex


namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(assertAfterIndexUpdate);

/**
 * Adjusts expiration setting on an index.
 */
void _processCollModIndexRequestExpireAfterSeconds(OperationContext* opCtx,
                                                   AutoGetCollection* autoColl,
                                                   const IndexDescriptor* idx,
                                                   long long indexExpireAfterSeconds,
                                                   boost::optional<long long>* newExpireSecs,
                                                   boost::optional<long long>* oldExpireSecs) {
    *newExpireSecs = indexExpireAfterSeconds;
    auto oldExpireSecsElement = idx->infoObj().getField("expireAfterSeconds");
    // If this collection was not previously TTL, inform the TTL monitor when we commit.
    if (!oldExpireSecsElement) {
        auto ttlCache = &TTLCollectionCache::get(opCtx->getServiceContext());
        const auto& coll = autoColl->getCollection();
        // Do not refer to 'idx' within this commit handler as it may be be invalidated by
        // IndexCatalog::refreshEntry().
        opCtx->recoveryUnit()->onCommit(
            [ttlCache, uuid = coll->uuid(), indexName = idx->indexName(), indexExpireAfterSeconds](
                auto _) {
                ttlCache->registerTTLInfo(
                    uuid, TTLCollectionCache::Info{indexName, /*isExpireAfterSecondsNaN=*/false});
            });

        // Change the value of "expireAfterSeconds" on disk.
        autoColl->getWritableCollection(opCtx)->updateTTLSetting(
            opCtx, idx->indexName(), indexExpireAfterSeconds);
        return;
    }

    // If the current `expireAfterSeconds` is NaN, it can never be equal to
    // 'indexExpireAfterSeconds'.
    if (oldExpireSecsElement.isNaN()) {
        // Setting *oldExpireSecs is mostly for informational purposes.
        // We could also use index_key_validate::kExpireAfterSecondsForInactiveTTLIndex but
        // 0 is more consistent with the previous safeNumberLong() behavior and avoids potential
        // showing the same value for the new and old values in the collMod response.
        *oldExpireSecs = 0;

        // Change the value of "expireAfterSeconds" on disk.
        autoColl->getWritableCollection(opCtx)->updateTTLSetting(
            opCtx, idx->indexName(), indexExpireAfterSeconds);

        // Keep the TTL information maintained by the TTLCollectionCache in sync so that we don't
        // try to fix up the TTL index during the next step-up.
        auto ttlCache = &TTLCollectionCache::get(opCtx->getServiceContext());
        const auto& coll = autoColl->getCollection();
        opCtx->recoveryUnit()->onCommit(
            [ttlCache, uuid = coll->uuid(), indexName = idx->indexName(), indexExpireAfterSeconds](
                auto _) { ttlCache->unsetTTLIndexExpireAfterSecondsNaN(uuid, indexName); });
        return;
    }

    // This collection is already TTL. Compare the requested value against the existing setting
    // before updating the catalog.
    *oldExpireSecs = oldExpireSecsElement.safeNumberLong();
    if (**oldExpireSecs != indexExpireAfterSeconds) {
        // Change the value of "expireAfterSeconds" on disk.
        autoColl->getWritableCollection(opCtx)->updateTTLSetting(
            opCtx, idx->indexName(), indexExpireAfterSeconds);
    }
}

/**
 * Adjusts hidden setting on an index.
 */
void _processCollModIndexRequestHidden(OperationContext* opCtx,
                                       AutoGetCollection* autoColl,
                                       const IndexDescriptor* idx,
                                       bool indexHidden,
                                       boost::optional<bool>* newHidden,
                                       boost::optional<bool>* oldHidden) {
    *newHidden = indexHidden;
    *oldHidden = idx->hidden();
    // Make sure when we set 'hidden' to false, we can remove the hidden field from catalog.
    if (*oldHidden != *newHidden) {
        autoColl->getWritableCollection(opCtx)->updateHiddenSetting(
            opCtx, idx->indexName(), indexHidden);
    }
}

/**
 * Returns set of keys for a document in an index.
 */
void getKeysForIndex(OperationContext* opCtx,
                     const CollectionPtr& collection,
                     const SortedDataIndexAccessMethod* accessMethod,
                     const BSONObj& doc,
                     KeyStringSet* keys) {
    SharedBufferFragmentBuilder pooledBuilder(KeyString::HeapBuilder::kHeapAllocatorDefaultBytes);

    accessMethod->getKeys(opCtx,
                          collection,
                          pooledBuilder,
                          doc,
                          InsertDeleteOptions::ConstraintEnforcementMode::kEnforceConstraints,
                          SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                          keys,
                          nullptr,       //  multikeyMetadataKeys
                          nullptr,       //  multikeyPaths
                          boost::none);  // loc
}

/**
 * Adjusts unique setting on an index to true.
 */
void _processCollModIndexRequestUnique(OperationContext* opCtx,
                                       AutoGetCollection* autoColl,
                                       const IndexDescriptor* idx,
                                       boost::optional<repl::OplogApplication::Mode> mode,
                                       boost::optional<bool>* newUnique) {
    invariant(!idx->unique(), str::stream() << "Index is already unique: " << idx->infoObj());
    const auto& collection = autoColl->getCollection();

    // Checks for duplicates for the 'applyOps' command. In the tenant migration case, assumes
    // similarly to initial sync that we don't need to perform this check in the destination
    // cluster.
    if (mode && *mode == repl::OplogApplication::Mode::kApplyOpsCmd) {
        auto duplicateRecordsList = scanIndexForDuplicates(opCtx, collection, idx);
        if (!duplicateRecordsList.empty()) {
            uassertStatusOK(buildConvertUniqueErrorStatus(opCtx, collection, duplicateRecordsList));
        }
    }

    *newUnique = true;
    autoColl->getWritableCollection(opCtx)->updateUniqueSetting(opCtx, idx->indexName(), true);
    // Resets 'prepareUnique' to false after converting to unique index;
    autoColl->getWritableCollection(opCtx)->updatePrepareUniqueSetting(
        opCtx, idx->indexName(), false);
}

/**
 * Adjusts prepareUnique setting on an index.
 */
void _processCollModIndexRequestPrepareUnique(OperationContext* opCtx,
                                              AutoGetCollection* autoColl,
                                              const IndexDescriptor* idx,
                                              bool indexPrepareUnique,
                                              boost::optional<bool>* newPrepareUnique,
                                              boost::optional<bool>* oldPrepareUnique) {
    *newPrepareUnique = indexPrepareUnique;
    *oldPrepareUnique = idx->prepareUnique();
    if (*oldPrepareUnique != *newPrepareUnique) {
        autoColl->getWritableCollection(opCtx)->updatePrepareUniqueSetting(
            opCtx, idx->indexName(), indexPrepareUnique);
    }
}

/**
 * Adjusts unique setting on an index to false.
 */
void _processCollModIndexRequestForceNonUnique(OperationContext* opCtx,
                                               AutoGetCollection* autoColl,
                                               const IndexDescriptor* idx,
                                               boost::optional<bool>* newForceNonUnique) {
    invariant(idx->unique(), str::stream() << "Index is already non-unique: " << idx->infoObj());

    *newForceNonUnique = true;
    autoColl->getWritableCollection(opCtx)->updateUniqueSetting(opCtx, idx->indexName(), false);
}

}  // namespace

void processCollModIndexRequest(OperationContext* opCtx,
                                AutoGetCollection* autoColl,
                                const ParsedCollModIndexRequest& collModIndexRequest,
                                boost::optional<IndexCollModInfo>* indexCollModInfo,
                                BSONObjBuilder* result,
                                boost::optional<repl::OplogApplication::Mode> mode) {
    auto idx = collModIndexRequest.idx;
    auto indexExpireAfterSeconds = collModIndexRequest.indexExpireAfterSeconds;
    auto indexHidden = collModIndexRequest.indexHidden;
    auto indexUnique = collModIndexRequest.indexUnique;
    auto indexPrepareUnique = collModIndexRequest.indexPrepareUnique;
    auto indexForceNonUnique = collModIndexRequest.indexForceNonUnique;

    // Return early if there are no index modifications requested.
    if (!indexExpireAfterSeconds && !indexHidden && !indexUnique && !indexPrepareUnique &&
        !indexForceNonUnique) {
        return;
    }

    boost::optional<long long> newExpireSecs;
    boost::optional<long long> oldExpireSecs;
    boost::optional<bool> newHidden;
    boost::optional<bool> oldHidden;
    boost::optional<bool> newUnique;
    boost::optional<bool> newPrepareUnique;
    boost::optional<bool> oldPrepareUnique;
    boost::optional<bool> newForceNonUnique;

    // TTL Index
    if (indexExpireAfterSeconds) {
        _processCollModIndexRequestExpireAfterSeconds(
            opCtx, autoColl, idx, *indexExpireAfterSeconds, &newExpireSecs, &oldExpireSecs);
    }


    // User wants to hide or unhide index.
    if (indexHidden) {
        _processCollModIndexRequestHidden(
            opCtx, autoColl, idx, *indexHidden, &newHidden, &oldHidden);
    }

    // User wants to convert an index to be unique.
    if (indexUnique) {
        invariant(*indexUnique);
        _processCollModIndexRequestUnique(opCtx, autoColl, idx, mode, &newUnique);
    }

    if (indexPrepareUnique) {
        _processCollModIndexRequestPrepareUnique(
            opCtx, autoColl, idx, *indexPrepareUnique, &newPrepareUnique, &oldPrepareUnique);
    }

    // User wants to convert an index back to be non-unique.
    if (indexForceNonUnique) {
        _processCollModIndexRequestForceNonUnique(opCtx, autoColl, idx, &newForceNonUnique);
    }

    *indexCollModInfo =
        IndexCollModInfo{!newExpireSecs ? boost::optional<Seconds>() : Seconds(*newExpireSecs),
                         !oldExpireSecs ? boost::optional<Seconds>() : Seconds(*oldExpireSecs),
                         newHidden,
                         oldHidden,
                         newUnique,
                         newPrepareUnique,
                         oldPrepareUnique,
                         newForceNonUnique,
                         idx->indexName()};

    // This matches the default for IndexCatalog::refreshEntry().
    auto flags = CreateIndexEntryFlags::kIsReady;

    // Update data format version in storage engine metadata for index.
    if (indexUnique || indexForceNonUnique) {
        flags = CreateIndexEntryFlags::kIsReady | CreateIndexEntryFlags::kUpdateMetadata;
        if (indexForceNonUnique) {
            flags = flags | CreateIndexEntryFlags::kForceUpdateMetadata;
        }
    }

    // Notify the index catalog that the definition of this index changed. This will invalidate the
    // local idx pointer. On rollback of this WUOW, the local var idx pointer will be valid again.
    autoColl->getWritableCollection(opCtx)->getIndexCatalog()->refreshEntry(
        opCtx, autoColl->getWritableCollection(opCtx), idx, flags);

    opCtx->recoveryUnit()->onCommit([oldExpireSecs,
                                     newExpireSecs,
                                     oldHidden,
                                     newHidden,
                                     newUnique,
                                     oldPrepareUnique,
                                     newPrepareUnique,
                                     newForceNonUnique,
                                     result](boost::optional<Timestamp>) {
        // add the fields to BSONObjBuilder result
        if (oldExpireSecs) {
            result->append("expireAfterSeconds_old", *oldExpireSecs);
        }
        if (newExpireSecs) {
            result->append("expireAfterSeconds_new", *newExpireSecs);
        }
        if (newHidden) {
            invariant(oldHidden);
            result->append("hidden_old", *oldHidden);
            result->append("hidden_new", *newHidden);
        }
        if (newUnique) {
            invariant(*newUnique);
            result->appendBool("unique_new", true);
        }
        if (newPrepareUnique) {
            invariant(oldPrepareUnique);
            result->append("prepareUnique_old", *oldPrepareUnique);
            result->append("prepareUnique_new", *newPrepareUnique);
        }
        if (newForceNonUnique) {
            invariant(*newForceNonUnique);
            result->appendBool("forceNonUnique_new", true);
        }
    });

    if (MONGO_unlikely(assertAfterIndexUpdate.shouldFail())) {
        LOGV2(20307, "collMod - assertAfterIndexUpdate fail point enabled");
        uasserted(50970, "trigger rollback after the index update");
    }
}

std::list<std::set<RecordId>> scanIndexForDuplicates(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    const IndexDescriptor* idx,
    boost::optional<KeyString::Value> firstKeyString) {
    auto entry = idx->getEntry();
    auto accessMethod = entry->accessMethod()->asSortedData();
    // Only scans for the duplicates on one key if 'firstKeyString' is provided.
    bool scanOneKey = static_cast<bool>(firstKeyString);

    // Starting point of index traversal.
    if (!firstKeyString) {
        auto keyStringVersion = accessMethod->getSortedDataInterface()->getKeyStringVersion();
        KeyString::Builder firstKeyStringBuilder(keyStringVersion,
                                                 BSONObj(),
                                                 entry->ordering(),
                                                 KeyString::Discriminator::kExclusiveBefore);
        firstKeyString = firstKeyStringBuilder.getValueCopy();
    }

    // Scans index for duplicates, comparing consecutive index entries.
    // KeyStrings will be in strictly increasing order because all keys are sorted and they are
    // in the format (Key, RID), and all RecordIDs are unique.
    DataThrottle dataThrottle(opCtx);
    dataThrottle.turnThrottlingOff();
    SortedDataInterfaceThrottleCursor indexCursor(opCtx, accessMethod, &dataThrottle);
    boost::optional<KeyStringEntry> prevIndexEntry;
    std::list<std::set<RecordId>> duplicateRecordsList;
    std::set<RecordId> duplicateRecords;
    for (auto indexEntry = indexCursor.seekForKeyString(opCtx, *firstKeyString); indexEntry;
         indexEntry = indexCursor.nextKeyString(opCtx)) {
        if (prevIndexEntry &&
            (indexEntry->loc.isLong()
                 ? indexEntry->keyString.compareWithoutRecordIdLong(prevIndexEntry->keyString)
                 : indexEntry->keyString.compareWithoutRecordIdStr(prevIndexEntry->keyString)) ==
                0) {
            if (duplicateRecords.empty()) {
                duplicateRecords.insert(prevIndexEntry->loc);
            }
            duplicateRecords.insert(indexEntry->loc);
        } else {
            if (!duplicateRecords.empty()) {
                // Adds the current group of violations with the same duplicate value.
                duplicateRecordsList.push_back(duplicateRecords);
                duplicateRecords.clear();
                if (scanOneKey) {
                    break;
                }
            }
        }
        prevIndexEntry = indexEntry;
    }
    if (!duplicateRecords.empty()) {
        duplicateRecordsList.push_back(duplicateRecords);
    }
    return duplicateRecordsList;
}

Status buildConvertUniqueErrorStatus(OperationContext* opCtx,
                                     const CollectionPtr& collection,
                                     const std::list<std::set<RecordId>>& duplicateRecordsList) {
    BSONArrayBuilder duplicateViolations;
    size_t violationsSize = 0;

    for (const auto& duplicateRecords : duplicateRecordsList) {
        BSONArrayBuilder currViolatingIds;
        for (const auto& recordId : duplicateRecords) {
            auto doc = collection->docFor(opCtx, recordId).value();
            auto id = doc["_id"];
            violationsSize += id.size();

            // Returns duplicate violations up to 8MB.
            if (violationsSize > BSONObjMaxUserSize / 2) {
                // Returns at least one violation.
                if (duplicateViolations.arrSize() == 0 && currViolatingIds.arrSize() == 0) {
                    currViolatingIds.append(id);
                }
                if (currViolatingIds.arrSize() > 0) {
                    duplicateViolations.append(BSON("ids" << currViolatingIds.arr()));
                }
                return Status(
                    CannotConvertIndexToUniqueInfo(duplicateViolations.arr()),
                    "Cannot convert the index to unique. Too many conflicting documents were "
                    "detected. Please resolve them and rerun collMod.");
            }
            currViolatingIds.append(id);
        }
        duplicateViolations.append(BSON("ids" << currViolatingIds.arr()));
    }
    return Status(CannotConvertIndexToUniqueInfo(duplicateViolations.arr()),
                  "Cannot convert the index to unique. Please resolve conflicting documents "
                  "before running collMod again.");
}

}  // namespace mongo
