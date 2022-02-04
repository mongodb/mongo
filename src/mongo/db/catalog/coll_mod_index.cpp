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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex

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
            [ttlCache, uuid = coll->uuid(), indexName = idx->indexName()](auto _) {
                ttlCache->registerTTLInfo(uuid, indexName);
            });

        // Change the value of "expireAfterSeconds" on disk.
        autoColl->getWritableCollection(opCtx)->updateTTLSetting(
            opCtx, idx->indexName(), indexExpireAfterSeconds);
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
 * Adjusts unique setting on an index.
 * An index can be converted to unique but removing the uniqueness property is not allowed.
 */
void _processCollModIndexRequestUnique(OperationContext* opCtx,
                                       AutoGetCollection* autoColl,
                                       const IndexDescriptor* idx,
                                       boost::optional<repl::OplogApplication::Mode> mode,
                                       const CollModWriteOpsTracker::Docs* docsForUniqueIndex,
                                       boost::optional<bool>* newUnique) {
    invariant(!idx->unique(), str::stream() << "Index is already unique: " << idx->infoObj());
    const auto& collection = autoColl->getCollection();

    // Checks for duplicates on the primary or for the 'applyOps' command. In the
    // tenant migration case, assumes similarly to initial sync that we don't need to perform this
    // check in the destination cluster.
    std::list<std::set<RecordId>> duplicateRecordsList;
    if (!mode) {
        auto entry = idx->getEntry();
        auto accessMethod = entry->accessMethod()->asSortedData();

        invariant(docsForUniqueIndex,
                  fmt::format("Unique index conversion requires valid set of changed docs from "
                              "side write tracker: {} {} {}",
                              collection->ns().toString(),
                              idx->indexName(),
                              collection->uuid().toString()));


        // Gather the keys for all the side write activity in a single set of unique keys.
        KeyStringSet keys;
        for (const auto& doc : *docsForUniqueIndex) {
            // The OpObserver records CRUD events in transactions that are about to be committed.
            // We should not assume that inserts/updates can be conflated with deletes.
            getKeysForIndex(opCtx, collection, accessMethod, doc, &keys);
        }

        // Search the index for duplicates using keys derived from side write documents.
        for (const auto& keyString : keys) {
            // Inserts and updates should generally refer to existing index entries, but since we
            // are recording uncommitted CRUD events, we should not always assume that searching
            // for 'keyString' in the index must return a a valid index entry.
            duplicateRecordsList.splice(duplicateRecordsList.end(),
                                        scanIndexForDuplicates(opCtx, collection, idx, keyString));
        }
    } else if (*mode == repl::OplogApplication::Mode::kApplyOpsCmd) {
        // We do not need to observe side writes under applyOps because applyOps runs under global
        // write access.
        duplicateRecordsList = scanIndexForDuplicates(opCtx, collection, idx);
    }
    if (!duplicateRecordsList.empty()) {
        uassertStatusOK(buildConvertUniqueErrorStatus(
            buildDuplicateViolations(opCtx, collection, duplicateRecordsList)));
    }

    *newUnique = true;
    autoColl->getWritableCollection(opCtx)->updateUniqueSetting(opCtx, idx->indexName());
    idx->getEntry()->accessMethod()->setEnforceDuplicateConstraints(false);
}

/**
 * Adjusts enforceDuplicateConstraints setting on an index.
 */
void _processCollModIndexRequestDisallowNewDuplicateKeys(
    OperationContext* opCtx,
    AutoGetCollection* autoColl,
    const IndexDescriptor* idx,
    bool indexDisallowNewDuplicateKeys,
    boost::optional<bool>* newDisallowNewDuplicateKeys,
    boost::optional<bool>* oldDisallowNewDuplicateKeys) {
    *newDisallowNewDuplicateKeys = indexDisallowNewDuplicateKeys;
    auto accessMethod = idx->getEntry()->accessMethod();
    *oldDisallowNewDuplicateKeys = accessMethod->isEnforcingDuplicateConstraints();
    if (*oldDisallowNewDuplicateKeys != *newDisallowNewDuplicateKeys) {
        accessMethod->setEnforceDuplicateConstraints(indexDisallowNewDuplicateKeys);
    }
}

}  // namespace

void processCollModIndexRequest(OperationContext* opCtx,
                                AutoGetCollection* autoColl,
                                const ParsedCollModIndexRequest& collModIndexRequest,
                                const CollModWriteOpsTracker::Docs* docsForUniqueIndex,
                                boost::optional<IndexCollModInfo>* indexCollModInfo,
                                BSONObjBuilder* result,
                                boost::optional<repl::OplogApplication::Mode> mode) {
    auto idx = collModIndexRequest.idx;
    auto indexExpireAfterSeconds = collModIndexRequest.indexExpireAfterSeconds;
    auto indexHidden = collModIndexRequest.indexHidden;
    auto indexUnique = collModIndexRequest.indexUnique;
    auto indexDisallowNewDuplicateKeys = collModIndexRequest.indexDisallowNewDuplicateKeys;

    // Return early if there are no index modifications requested.
    if (!indexExpireAfterSeconds && !indexHidden && !indexUnique &&
        !indexDisallowNewDuplicateKeys) {
        return;
    }

    boost::optional<long long> newExpireSecs;
    boost::optional<long long> oldExpireSecs;
    boost::optional<bool> newHidden;
    boost::optional<bool> oldHidden;
    boost::optional<bool> newUnique;
    boost::optional<bool> newDisallowNewDuplicateKeys;
    boost::optional<bool> oldDisallowNewDuplicateKeys;

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
        _processCollModIndexRequestUnique(
            opCtx, autoColl, idx, mode, docsForUniqueIndex, &newUnique);
    }

    if (indexDisallowNewDuplicateKeys) {
        _processCollModIndexRequestDisallowNewDuplicateKeys(opCtx,
                                                            autoColl,
                                                            idx,
                                                            *indexDisallowNewDuplicateKeys,
                                                            &newDisallowNewDuplicateKeys,
                                                            &oldDisallowNewDuplicateKeys);
    }

    *indexCollModInfo =
        IndexCollModInfo{!newExpireSecs ? boost::optional<Seconds>() : Seconds(*newExpireSecs),
                         !oldExpireSecs ? boost::optional<Seconds>() : Seconds(*oldExpireSecs),
                         newHidden,
                         oldHidden,
                         newUnique,
                         oldDisallowNewDuplicateKeys,
                         newDisallowNewDuplicateKeys,
                         idx->indexName()};

    // This matches the default for IndexCatalog::refreshEntry().
    auto flags = CreateIndexEntryFlags::kIsReady;

    // Update data format version in storage engine metadata for index.
    if (indexUnique) {
        flags = CreateIndexEntryFlags::kIsReady | CreateIndexEntryFlags::kUpdateMetadata;
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
                                     oldDisallowNewDuplicateKeys,
                                     newDisallowNewDuplicateKeys,
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
        if (newDisallowNewDuplicateKeys) {
            // Unlike other fields, 'disallowNewDuplicateKeys' can have the same old and new values.
            invariant(oldDisallowNewDuplicateKeys);
            result->append("disallowNewDuplicateKeys_old", *oldDisallowNewDuplicateKeys);
            result->append("disallowNewDuplicateKeys_new", *newDisallowNewDuplicateKeys);
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
            indexEntry->keyString.compareWithoutRecordIdLong(prevIndexEntry->keyString) == 0) {
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

BSONArray buildDuplicateViolations(OperationContext* opCtx,
                                   const CollectionPtr& collection,
                                   const std::list<std::set<RecordId>>& duplicateRecordsList) {
    BSONArrayBuilder duplicateViolations;
    for (const auto& duplicateRecords : duplicateRecordsList) {
        BSONArrayBuilder currViolatingIds;
        for (const auto& recordId : duplicateRecords) {
            auto doc = collection->docFor(opCtx, recordId).value();
            currViolatingIds.append(doc["_id"]);
        }
        duplicateViolations.append(BSON("ids" << currViolatingIds.arr()));
    }
    return duplicateViolations.arr();
}

Status buildConvertUniqueErrorStatus(const BSONArray& violations) {
    return Status(CannotConvertIndexToUniqueInfo(violations),
                  "Cannot convert the index to unique. Please resolve conflicting documents "
                  "before running collMod again.");
}

}  // namespace mongo
