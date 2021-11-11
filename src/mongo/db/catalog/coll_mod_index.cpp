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

#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/db/catalog/throttle_cursor.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/ttl_collection_cache.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"

namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(assertAfterIndexUpdate);

class CollModResultChange : public RecoveryUnit::Change {
public:
    CollModResultChange(const BSONElement& oldExpireSecs,
                        const BSONElement& newExpireSecs,
                        const BSONElement& oldHidden,
                        const BSONElement& newHidden,
                        const BSONElement& newUnique,
                        BSONObjBuilder* result)
        : _oldExpireSecs(oldExpireSecs),
          _newExpireSecs(newExpireSecs),
          _oldHidden(oldHidden),
          _newHidden(newHidden),
          _newUnique(newUnique),
          _result(result) {}

    void commit(boost::optional<Timestamp>) override {
        // add the fields to BSONObjBuilder result
        if (!_oldExpireSecs.eoo()) {
            _result->appendAs(_oldExpireSecs, "expireAfterSeconds_old");
        }
        if (!_newExpireSecs.eoo()) {
            _result->appendAs(_newExpireSecs, "expireAfterSeconds_new");
        }
        if (!_newHidden.eoo()) {
            bool oldValue = _oldHidden.eoo() ? false : _oldHidden.booleanSafe();
            _result->append("hidden_old", oldValue);
            _result->appendAs(_newHidden, "hidden_new");
        }
        if (!_newUnique.eoo()) {
            invariant(_newUnique.trueValue());
            _result->appendBool("unique_new", true);
        }
    }

    void rollback() override {}

private:
    const BSONElement _oldExpireSecs;
    const BSONElement _newExpireSecs;
    const BSONElement _oldHidden;
    const BSONElement _newHidden;
    const BSONElement _newUnique;
    BSONObjBuilder* _result;
};

/**
 * Adjusts expiration setting on an index.
 */
void _processCollModIndexRequestExpireAfterSeconds(OperationContext* opCtx,
                                                   AutoGetCollection* autoColl,
                                                   const IndexDescriptor* idx,
                                                   BSONElement indexExpireAfterSeconds,
                                                   BSONElement* newExpireSecs,
                                                   BSONElement* oldExpireSecs) {
    *newExpireSecs = indexExpireAfterSeconds;
    *oldExpireSecs = idx->infoObj().getField("expireAfterSeconds");
    // If this collection was not previously TTL, inform the TTL monitor when we commit.
    if (oldExpireSecs->eoo()) {
        auto ttlCache = &TTLCollectionCache::get(opCtx->getServiceContext());
        const auto& coll = autoColl->getCollection();
        // Do not refer to 'idx' within this commit handler as it may be be invalidated by
        // IndexCatalog::refreshEntry().
        opCtx->recoveryUnit()->onCommit(
            [ttlCache, uuid = coll->uuid(), indexName = idx->indexName()](auto _) {
                ttlCache->registerTTLInfo(uuid, indexName);
            });
    }
    if (SimpleBSONElementComparator::kInstance.evaluate(*oldExpireSecs != *newExpireSecs)) {
        // Change the value of "expireAfterSeconds" on disk.
        autoColl->getWritableCollection()->updateTTLSetting(
            opCtx, idx->indexName(), newExpireSecs->safeNumberLong());
    }
}

/**
 * Adjusts hidden setting on an index.
 */
void _processCollModIndexRequestHidden(OperationContext* opCtx,
                                       AutoGetCollection* autoColl,
                                       const IndexDescriptor* idx,
                                       BSONElement indexHidden,
                                       BSONElement* newHidden,
                                       BSONElement* oldHidden) {
    *newHidden = indexHidden;
    *oldHidden = idx->infoObj().getField("hidden");
    // Make sure when we set 'hidden' to false, we can remove the hidden field from catalog.
    if (SimpleBSONElementComparator::kInstance.evaluate(*oldHidden != *newHidden)) {
        autoColl->getWritableCollection()->updateHiddenSetting(
            opCtx, idx->indexName(), newHidden->booleanSafe());
    }
}

/**
 * Adjusts unique setting on an index.
 * An index can be converted to unique but removing the uniqueness property is not allowed.
 */
void _processCollModIndexRequestUnique(OperationContext* opCtx,
                                       AutoGetCollection* autoColl,
                                       const IndexDescriptor* idx,
                                       boost::optional<repl::OplogApplication::Mode> mode,
                                       BSONElement indexUnique,
                                       BSONElement* newUnique) {
    // Do not update catalog if index is already unique.
    if (idx->infoObj().getField("unique").trueValue()) {
        return;
    }

    // Checks for duplicates on the primary or for the 'applyOps' command.
    // TODO(SERVER-61356): revisit this condition for tenant migration, which uses kInitialSync.
    if (!mode || *mode == repl::OplogApplication::Mode::kApplyOpsCmd) {
        scanIndexForDuplicates(opCtx, autoColl->getCollection(), idx);
    }

    *newUnique = indexUnique;
    autoColl->getWritableCollection()->updateUniqueSetting(opCtx, idx->indexName());
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

    // Return early if there are no index modifications requested.
    if (!indexExpireAfterSeconds && !indexHidden && !indexUnique) {
        return;
    }

    BSONElement newExpireSecs = {};
    BSONElement oldExpireSecs = {};
    BSONElement newHidden = {};
    BSONElement oldHidden = {};
    BSONElement newUnique = {};

    // TTL Index
    if (indexExpireAfterSeconds) {
        _processCollModIndexRequestExpireAfterSeconds(
            opCtx, autoColl, idx, indexExpireAfterSeconds, &newExpireSecs, &oldExpireSecs);
    }


    // User wants to hide or unhide index.
    if (indexHidden) {
        _processCollModIndexRequestHidden(
            opCtx, autoColl, idx, indexHidden, &newHidden, &oldHidden);
    }

    // User wants to convert an index to be unique.
    if (indexUnique) {
        _processCollModIndexRequestUnique(opCtx, autoColl, idx, mode, indexUnique, &newUnique);
    }

    *indexCollModInfo = IndexCollModInfo{
        !indexExpireAfterSeconds ? boost::optional<Seconds>()
                                 : Seconds(newExpireSecs.safeNumberLong()),
        !indexExpireAfterSeconds || oldExpireSecs.eoo() ? boost::optional<Seconds>()
                                                        : Seconds(oldExpireSecs.safeNumberLong()),
        !indexHidden ? boost::optional<bool>() : newHidden.booleanSafe(),
        !indexHidden ? boost::optional<bool>() : oldHidden.booleanSafe(),
        !indexUnique ? boost::optional<bool>() : newUnique.booleanSafe(),
        idx->indexName()};

    // This matches the default for IndexCatalog::refreshEntry().
    auto flags = CreateIndexEntryFlags::kIsReady;

    // Update data format version in storage engine metadata for index.
    if (indexUnique) {
        flags = CreateIndexEntryFlags::kIsReady | CreateIndexEntryFlags::kUpdateMetadata;
    }

    // Notify the index catalog that the definition of this index changed. This will invalidate the
    // local idx pointer. On rollback of this WUOW, the local var idx pointer will be valid again.
    autoColl->getWritableCollection()->getIndexCatalog()->refreshEntry(
        opCtx, autoColl->getWritableCollection(), idx, flags);

    opCtx->recoveryUnit()->registerChange(std::make_unique<CollModResultChange>(
        oldExpireSecs, newExpireSecs, oldHidden, newHidden, newUnique, result));

    if (MONGO_unlikely(assertAfterIndexUpdate.shouldFail())) {
        LOGV2(20307, "collMod - assertAfterIndexUpdate fail point enabled");
        uasserted(50970, "trigger rollback after the index update");
    }
}

void scanIndexForDuplicates(OperationContext* opCtx,
                            const CollectionPtr& collection,
                            const IndexDescriptor* idx) {
    auto entry = idx->getEntry();
    auto accessMethod = entry->accessMethod();

    // Starting point of index traversal.
    auto keyStringVersion = accessMethod->getSortedDataInterface()->getKeyStringVersion();
    KeyString::Builder firstKeyStringBuilder(
        keyStringVersion, BSONObj(), entry->ordering(), KeyString::Discriminator::kExclusiveBefore);
    KeyString::Value firstKeyString = firstKeyStringBuilder.getValueCopy();

    // Scan index for duplicates, comparing consecutive index entries.
    // KeyStrings will be in strictly increasing order because all keys are sorted and they are
    // in the format (Key, RID), and all RecordIDs are unique.
    DataThrottle dataThrottle(opCtx);
    dataThrottle.turnThrottlingOff();
    SortedDataInterfaceThrottleCursor indexCursor(opCtx, accessMethod, &dataThrottle);
    boost::optional<KeyStringEntry> prevIndexEntry;
    for (auto indexEntry = indexCursor.seekForKeyString(opCtx, firstKeyString); indexEntry;
         indexEntry = indexCursor.nextKeyString(opCtx)) {

        if (prevIndexEntry &&
            indexEntry->keyString.compareWithoutRecordIdLong(prevIndexEntry->keyString) == 0) {
            auto dupKeyErrorStatus =
                buildDupKeyErrorStatus(opCtx, indexEntry->keyString, entry->ordering(), idx);
            auto firstDoc = collection->docFor(opCtx, prevIndexEntry->loc);
            auto secondDoc = collection->docFor(opCtx, indexEntry->loc);
            uassertStatusOK(dupKeyErrorStatus.withContext(
                str::stream() << "Failed to convert index to unique. firstRecordId: "
                              << prevIndexEntry->loc << "; firstDoc: " << firstDoc.value()
                              << "; secondRecordId" << indexEntry->loc << "; secondDoc: "
                              << secondDoc.value() << "; collectionUUID: " << collection->uuid()));
        }

        prevIndexEntry = indexEntry;
    }
}

}  // namespace mongo
