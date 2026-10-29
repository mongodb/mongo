// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/shard_role/shard_catalog/coll_mod_index.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index_key_validate.h"
#include "mongo/db/shard_role/shard_catalog/cannot_convert_index_to_unique_info.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/ttl/ttl_collection_cache.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex


namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(assertAfterIndexUpdate);

/**
 * Adjusts expiration setting on an index.
 */
void _processCollModIndexRequestExpireAfterSeconds(OperationContext* opCtx,
                                                   Collection* writableColl,
                                                   const IndexDescriptor* idx,
                                                   long long indexExpireAfterSeconds,
                                                   boost::optional<long long>* newExpireSecs,
                                                   boost::optional<long long>* oldExpireSecs) {
    *newExpireSecs = indexExpireAfterSeconds;
    auto oldExpireSecsElement = idx->infoObj().getField("expireAfterSeconds");
    // If this collection was not previously TTL, inform the TTL monitor when we commit.
    if (!oldExpireSecsElement) {
        auto ttlCache = &TTLCollectionCache::get(opCtx->getServiceContext());
        // Do not refer to 'idx' within this commit handler as it may be be invalidated by
        // IndexCatalog::refreshEntry().
        shard_role_details::getRecoveryUnit(opCtx)->onCommit(
            [ttlCache, uuid = writableColl->uuid(), indexName = idx->indexName()](
                OperationContext*, boost::optional<Timestamp>) {
                // We assume the expireAfterSeconds field is valid, because we've already done
                // validation of this field.
                ttlCache->registerTTLInfo(
                    uuid,
                    TTLCollectionCache::Info{
                        indexName, TTLCollectionCache::Info::ExpireAfterSecondsType::kInt});
            });

        // Change the value of "expireAfterSeconds" on disk.
        writableColl->updateTTLSetting(opCtx, idx->indexName(), indexExpireAfterSeconds);
        return;
    }

    // If the current `expireAfterSeconds` is invalid (cannot safely fit into a 32 bit integer), it
    // can never be equal to 'indexExpireAfterSeconds'.
    if (auto status = index_key_validate::validateExpireAfterSeconds(
            oldExpireSecsElement,
            index_key_validate::ValidateExpireAfterSecondsMode::kSecondaryTTLIndex);
        !status.isOK()) {
        // Setting *oldExpireSecs is mostly for informational purposes.
        // We could also use index_key_validate::kExpireAfterSecondsForInactiveTTLIndex but
        // 0 is more consistent with the previous safeNumberLong() behavior and avoids potential
        // showing the same value for the new and old values in the collMod response.
        *oldExpireSecs = 0;

        // Change the value of "expireAfterSeconds" on disk.
        writableColl->updateTTLSetting(opCtx, idx->indexName(), indexExpireAfterSeconds);

        // Keep the TTL information maintained by the TTLCollectionCache in sync so that we don't
        // try to fix up the TTL index during the next step-up.
        auto ttlCache = &TTLCollectionCache::get(opCtx->getServiceContext());
        shard_role_details::getRecoveryUnit(opCtx)->onCommit(
            [ttlCache, uuid = writableColl->uuid(), indexName = idx->indexName()](
                OperationContext*, boost::optional<Timestamp>) {
                ttlCache->setTTLIndexExpireAfterSecondsType(
                    uuid, indexName, TTLCollectionCache::Info::ExpireAfterSecondsType::kInt);
            });
        return;
    }

    // This collection is already TTL. Compare the requested value against the existing setting
    // before updating the catalog. Also update if the stored type needs normalization to an
    // integer type (e.g., the old value is a double like 3600.0).
    *oldExpireSecs = oldExpireSecsElement.safeNumberLong();
    bool shouldUpdateCatalog = **oldExpireSecs != indexExpireAfterSeconds ||
        (oldExpireSecsElement.type() != BSONType::numberInt &&
         oldExpireSecsElement.type() != BSONType::numberLong);
    if (shouldUpdateCatalog) {
        // Change the value of "expireAfterSeconds" on disk.
        auto ttlCache = &TTLCollectionCache::get(opCtx->getServiceContext());
        shard_role_details::getRecoveryUnit(opCtx)->onCommit(
            [ttlCache, uuid = writableColl->uuid(), indexName = idx->indexName()](
                OperationContext*, boost::optional<Timestamp>) {
                ttlCache->setTTLIndexExpireAfterSecondsType(
                    uuid, indexName, TTLCollectionCache::Info::ExpireAfterSecondsType::kInt);
            });
        writableColl->updateTTLSetting(opCtx, idx->indexName(), indexExpireAfterSeconds);
    }
}

/**
 * Adjusts hidden setting on an index.
 */
void _processCollModIndexRequestHidden(OperationContext* opCtx,
                                       Collection* writableColl,
                                       const IndexDescriptor* idx,
                                       bool indexHidden,
                                       boost::optional<bool>* newHidden,
                                       boost::optional<bool>* oldHidden) {
    *newHidden = indexHidden;
    *oldHidden = idx->hidden();
    // Make sure when we set 'hidden' to false, we can remove the hidden field from catalog.
    if (*oldHidden != *newHidden) {
        writableColl->updateHiddenSetting(opCtx, idx->indexName(), indexHidden);
    }
}

/**
 * Adjusts unique setting on an index to true.
 */
void _processCollModIndexRequestUnique(OperationContext* opCtx,
                                       Collection* writableColl,
                                       const IndexDescriptor* idx,
                                       boost::optional<repl::OplogApplication::Mode> mode,
                                       boost::optional<bool>* newUnique) {
    invariant(!idx->unique(), str::stream() << "Index is already unique: " << idx->infoObj());

    // Checks for duplicates for the 'applyOps' command.
    if (mode && *mode == repl::OplogApplication::Mode::kApplyOpsCmd) {
        auto duplicateRecords = scanIndexForDuplicates(opCtx, writableColl, idx);
        if (!duplicateRecords.empty()) {
            uassertStatusOK(buildConvertUniqueErrorStatus(opCtx, writableColl, duplicateRecords));
        }
    }

    *newUnique = true;
    writableColl->updateUniqueSetting(opCtx, idx->indexName(), true);
    // Resets 'prepareUnique' to false after converting to unique index;
    writableColl->updatePrepareUniqueSetting(opCtx, idx->indexName(), false);
}

/**
 * Adjusts prepareUnique setting on an index.
 */
void _processCollModIndexRequestPrepareUnique(OperationContext* opCtx,
                                              Collection* writableColl,
                                              const IndexDescriptor* idx,
                                              bool indexPrepareUnique,
                                              boost::optional<bool>* newPrepareUnique,
                                              boost::optional<bool>* oldPrepareUnique) {
    *newPrepareUnique = indexPrepareUnique;
    *oldPrepareUnique = idx->prepareUnique();
    if (*oldPrepareUnique != *newPrepareUnique) {
        writableColl->updatePrepareUniqueSetting(opCtx, idx->indexName(), indexPrepareUnique);
    }
}

/**
 * Adjusts unique setting on an index to false.
 */
void _processCollModIndexRequestForceNonUnique(OperationContext* opCtx,
                                               Collection* writableColl,
                                               const IndexDescriptor* idx,
                                               boost::optional<bool>* newForceNonUnique) {
    invariant(idx->unique(), str::stream() << "Index is already non-unique: " << idx->infoObj());

    *newForceNonUnique = true;
    writableColl->updateUniqueSetting(opCtx, idx->indexName(), false);
}

}  // namespace

void processCollModIndexRequest(OperationContext* opCtx,
                                Collection* writableColl,
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
            opCtx, writableColl, idx, *indexExpireAfterSeconds, &newExpireSecs, &oldExpireSecs);
    }


    // User wants to hide or unhide index.
    if (indexHidden) {
        _processCollModIndexRequestHidden(
            opCtx, writableColl, idx, *indexHidden, &newHidden, &oldHidden);
    }

    // User wants to convert an index to be unique.
    if (indexUnique) {
        invariant(*indexUnique);
        _processCollModIndexRequestUnique(opCtx, writableColl, idx, mode, &newUnique);
    }

    if (indexPrepareUnique) {
        _processCollModIndexRequestPrepareUnique(
            opCtx, writableColl, idx, *indexPrepareUnique, &newPrepareUnique, &oldPrepareUnique);
    }

    // User wants to convert an index back to be non-unique.
    if (indexForceNonUnique) {
        _processCollModIndexRequestForceNonUnique(opCtx, writableColl, idx, &newForceNonUnique);
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
    auto entry = writableColl->getIndexCatalog()->findIndexByName(opCtx, idx->indexName());
    writableColl->getIndexCatalog()->refreshEntry(opCtx, writableColl, entry, flags);

    shard_role_details::getRecoveryUnit(opCtx)->onCommit(
        [oldExpireSecs,
         newExpireSecs,
         oldHidden,
         newHidden,
         newUnique,
         oldPrepareUnique,
         newPrepareUnique,
         newForceNonUnique,
         result](OperationContext*, boost::optional<Timestamp>) {
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

std::vector<std::vector<RecordId>> scanIndexForDuplicates(OperationContext* opCtx,
                                                          const Collection* coll,
                                                          const IndexDescriptor* idx) {
    auto entry = coll->getIndexCatalog()->findIndexByName(opCtx, idx->indexName());
    auto accessMethod = entry->accessMethod()->asSortedData();

    // Scans index for duplicates, comparing consecutive index entries.
    // KeyStrings will be in strictly increasing order because all keys are sorted and they are
    // in the format (Key, RID), and all RecordIDs are unique.
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    auto indexCursor = accessMethod->newCursor(opCtx, ru);
    boost::optional<KeyStringEntry> prevIndexEntry;
    std::vector<std::vector<RecordId>> duplicateRecords;
    std::vector<RecordId> curDuplicateRecords;
    size_t duplicateRecordIdMemUsage{0};
    size_t indexEntryCount{0};

    Date_t lastLogTime = Date_t::now();
    for (auto indexEntry = indexCursor->nextKeyString(ru); indexEntry;
         indexEntry = indexCursor->nextKeyString(ru)) {
        if (prevIndexEntry &&
            indexEntry->keyString.compareWithoutRecordId(prevIndexEntry->keyString,
                                                         indexEntry->loc.keyFormat()) == 0) {
            if (curDuplicateRecords.empty()) {
                curDuplicateRecords.push_back(std::move(prevIndexEntry->loc));
                duplicateRecordIdMemUsage += curDuplicateRecords.back().memUsage();
            }
            curDuplicateRecords.push_back(std::move(indexEntry->loc));
            duplicateRecordIdMemUsage += curDuplicateRecords.back().memUsage();
        } else {
            if (!curDuplicateRecords.empty()) {
                // Adds the current group of violations with the same duplicate value.
                duplicateRecords.push_back(std::move(curDuplicateRecords));
                curDuplicateRecords.clear();
            }
        }
        prevIndexEntry = indexEntry;
        ++indexEntryCount;

        // Return up to 16MB of RecordIds, to keep memory usage under control. Even a single
        // duplicate RecordId causes the command to fail, but we try to return as many RecordIds as
        // we safely can to help the user to diagnose the problem.
        static constexpr int64_t kMaxDuplicateRecordIdMemUsage = BSONObjMaxUserSize;
        if (duplicateRecordIdMemUsage >= kMaxDuplicateRecordIdMemUsage) {
            break;
        }

        // This was a rough heuristic that was found that 300 values with the index cursor would
        // take ~200 milliseconds with calling checkForInterrupt(). We made the checkForInterrupt()
        // occur after 150 index entries to be conservative.
        if (!(indexEntryCount % 150)) {
            opCtx->checkForInterrupt();
        }

        Date_t now = Date_t::now();
        if (now - lastLogTime >= Seconds(1)) {
            LOGV2(9335700,
                  "Number of keys scanned for duplicates in unique index conversion",
                  "numKeysIterated"_attr = indexEntryCount);
            lastLogTime = now;
        }
    }
    if (!curDuplicateRecords.empty()) {
        duplicateRecords.push_back(std::move(curDuplicateRecords));
    }
    return duplicateRecords;
}

Status buildConvertUniqueErrorStatus(OperationContext* opCtx,
                                     const Collection* collection,
                                     const std::vector<std::vector<RecordId>>& duplicateRecords) {
    BSONArrayBuilder duplicateViolations;
    size_t violationsSize = 0;

    for (const auto& recordsWithDuplicateKeys : duplicateRecords) {
        BSONArrayBuilder currViolatingIds;
        for (const auto& recordId : recordsWithDuplicateKeys) {
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
