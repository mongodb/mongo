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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/exec/working_set_common.h"

#include <boost/iterator/transform_iterator.hpp>

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/execution_context.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/logv2/log.h"

namespace mongo {

namespace {
std::string indexKeyVectorDebugString(const std::vector<IndexKeyDatum>& keyData) {
    StringBuilder sb;
    sb << "[";
    if (keyData.size() > 0) {
        auto it = keyData.begin();
        sb << "(key: " << redact(it->keyData) << ", index key pattern: " << it->indexKeyPattern
           << ")";
        while (++it != keyData.end()) {
            sb << ", (key: " << redact(it->keyData)
               << ", index key pattern: " << it->indexKeyPattern << ")";
        }
    }
    sb << "]";
    return sb.str();
}
}  // namespace

// static
bool WorkingSetCommon::fetch(OperationContext* opCtx,
                             WorkingSet* workingSet,
                             WorkingSetID id,
                             SeekableRecordCursor* cursor,
                             const CollectionPtr& collection,
                             const NamespaceString& ns) {
    WorkingSetMember* member = workingSet->get(id);

    // We should have a RecordId but need to retrieve the obj. Get the obj now and reset all WSM
    // state appropriately.
    invariant(member->hasRecordId());

    auto record = cursor->seekExact(member->recordId);
    if (!record) {
        // The record referenced by this index entry is gone. If the query yielded some time after
        // we first examined the index entry, then it's likely that the record was deleted while we
        // were yielding. However, if the snapshot id hasn't changed since the index lookup, then
        // there could not have been a yield, meaning the document we are searching for has been
        // deleted.
        // One possibility is that the record was deleted by a prepared transaction, but if we are
        // not ignoring prepare conflicts, then this definitely indicates an error.
        std::vector<IndexKeyDatum>::iterator keyDataIt;
        if (member->getState() == WorkingSetMember::RID_AND_IDX &&
            opCtx->recoveryUnit()->getPrepareConflictBehavior() ==
                PrepareConflictBehavior::kEnforce &&
            (keyDataIt = std::find_if(member->keyData.begin(),
                                      member->keyData.end(),
                                      [currentSnapshotId = opCtx->recoveryUnit()->getSnapshotId()](
                                          const auto& keyDatum) {
                                          return keyDatum.snapshotId == currentSnapshotId;
                                      })) != member->keyData.end()) {
            auto indexKeyEntryToObjFn = [](const IndexKeyDatum& ikd) {
                BSONObjBuilder builder;
                // Rehydrate the index key fields to prevent duplicate "" fields from being logged.
                builder.append(
                    "key"_sd,
                    redact(IndexKeyEntry::rehydrateKey(ikd.indexKeyPattern, ikd.keyData)));
                builder.append("pattern"_sd, ikd.indexKeyPattern);
                return builder.obj();
            };
            LOGV2_ERROR_OPTIONS(
                4615603,
                {logv2::UserAssertAfterLog(ErrorCodes::DataCorruptionDetected)},
                "Erroneous index key found with reference to non-existent record id. Consider "
                "dropping and then re-creating the index and then running the validate command "
                "on the collection.",
                "namespace"_attr = ns,
                "recordId"_attr = member->recordId,
                "indexKeyData"_attr = logv2::seqLog(
                    boost::make_transform_iterator(member->keyData.begin(), indexKeyEntryToObjFn),
                    boost::make_transform_iterator(member->keyData.end(), indexKeyEntryToObjFn)));
        }
        return false;
    }

    auto currentSnapshotId = opCtx->recoveryUnit()->getSnapshotId();
    member->resetDocument(currentSnapshotId, record->data.releaseToBson());

    // Make sure that all of the keyData is still valid for this copy of the document.  This ensures
    // both that index-provided filters and sort orders still hold.
    //
    // TODO provide a way for the query planner to opt out of this checking if it is unneeded due to
    // the structure of the plan.
    if (member->getState() == WorkingSetMember::RID_AND_IDX) {
        auto& executionCtx = StorageExecutionContext::get(opCtx);
        for (size_t i = 0; i < member->keyData.size(); i++) {
            auto&& memberKey = member->keyData[i];
            // If this key was obtained in the current snapshot, then move on to the next key. There
            // is no way for this key to be inconsistent with the document it points to.
            if (memberKey.snapshotId == currentSnapshotId) {
                continue;
            }

            auto keys = executionCtx.keys();
            SharedBufferFragmentBuilder pool(KeyString::HeapBuilder::kHeapAllocatorDefaultBytes);
            // There's no need to compute the prefixes of the indexed fields that cause the
            // index to be multikey when ensuring the keyData is still valid.
            KeyStringSet* multikeyMetadataKeys = nullptr;
            MultikeyPaths* multikeyPaths = nullptr;
            auto* iam = workingSet->retrieveIndexAccessMethod(memberKey.indexId);
            iam->getKeys(opCtx,
                         collection,
                         pool,
                         member->doc.value().toBson(),
                         IndexAccessMethod::GetKeysMode::kEnforceConstraints,
                         IndexAccessMethod::GetKeysContext::kValidatingKeys,
                         keys.get(),
                         multikeyMetadataKeys,
                         multikeyPaths,
                         member->recordId,
                         IndexAccessMethod::kNoopOnSuppressedErrorFn);
            KeyString::HeapBuilder keyString(iam->getSortedDataInterface()->getKeyStringVersion(),
                                             memberKey.keyData,
                                             iam->getSortedDataInterface()->getOrdering(),
                                             member->recordId);
            if (!keys->count(keyString.release())) {
                // document would no longer be at this position in the index.
                return false;
            }
        }
    }

    member->keyData.clear();
    workingSet->transitionToRecordIdAndObj(id);
    return true;
}

}  // namespace mongo
