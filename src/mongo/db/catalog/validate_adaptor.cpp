/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/validate_adaptor.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_consistency.h"
#include "mongo/db/catalog/throttle_cursor.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/wildcard_access_method.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/storage/execution_context.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/object_check.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/testing_proctor.h"

namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(crashOnMultikeyValidateFailure);

// Set limit for size of corrupted records that will be reported.
const long long kMaxErrorSizeBytes = 1 * 1024 * 1024;
const long long kInterruptIntervalNumRecords = 4096;
const long long kInterruptIntervalNumBytes = 50 * 1024 * 1024;  // 50MB.

}  // namespace

Status ValidateAdaptor::validateRecord(OperationContext* opCtx,
                                       const RecordId& recordId,
                                       const RecordData& record,
                                       size_t* dataSize,
                                       ValidateResults* results) {
    const Status status = validateBSON(record.data(), record.size());
    if (!status.isOK())
        return status;

    BSONObj recordBson = record.toBson();
    *dataSize = recordBson.objsize();

    if (MONGO_unlikely(_validateState->extraLoggingForTest())) {
        LOGV2(4666601, "[validate]", "recordId"_attr = recordId, "recordData"_attr = recordBson);
    }

    const CollectionPtr& coll = _validateState->getCollection();
    if (!coll->getIndexCatalog()->haveAnyIndexes()) {
        return status;
    }

    auto& executionCtx = StorageExecutionContext::get(opCtx);
    SharedBufferFragmentBuilder pool(KeyString::HeapBuilder::kHeapAllocatorDefaultBytes);

    for (const auto& index : _validateState->getIndexes()) {
        const IndexDescriptor* descriptor = index->descriptor();
        const IndexAccessMethod* iam = index->accessMethod();

        if (descriptor->isPartial() && !index->getFilterExpression()->matchesBSON(recordBson))
            continue;

        auto documentKeySet = executionCtx.keys();
        auto multikeyMetadataKeys = executionCtx.multikeyMetadataKeys();
        auto documentMultikeyPaths = executionCtx.multikeyPaths();

        iam->getKeys(opCtx,
                     coll,
                     pool,
                     recordBson,
                     IndexAccessMethod::GetKeysMode::kEnforceConstraints,
                     IndexAccessMethod::GetKeysContext::kAddingKeys,
                     documentKeySet.get(),
                     multikeyMetadataKeys.get(),
                     documentMultikeyPaths.get(),
                     recordId,
                     IndexAccessMethod::kNoopOnSuppressedErrorFn);

        bool shouldBeMultikey = iam->shouldMarkIndexAsMultikey(
            documentKeySet->size(),
            {multikeyMetadataKeys->begin(), multikeyMetadataKeys->end()},
            *documentMultikeyPaths);

        if (!index->isMultikey(opCtx, coll) && shouldBeMultikey) {
            if (_validateState->fixErrors()) {
                writeConflictRetry(opCtx, "setIndexAsMultikey", coll->ns().ns(), [&] {
                    WriteUnitOfWork wuow(opCtx);
                    coll->getIndexCatalog()->setMultikeyPaths(
                        opCtx, coll, descriptor, *multikeyMetadataKeys, *documentMultikeyPaths);
                    wuow.commit();
                });

                LOGV2(4614700,
                      "Index set to multikey",
                      "indexName"_attr = descriptor->indexName(),
                      "collection"_attr = coll->ns().ns());
                results->warnings.push_back(str::stream() << "Index " << descriptor->indexName()
                                                          << " set to multikey.");
                results->repaired = true;
            } else {
                auto& curRecordResults = (results->indexResultsMap)[descriptor->indexName()];
                std::string msg = str::stream() << "Index " << descriptor->indexName()
                                                << " is not multikey but has more than one"
                                                << " key in document " << recordId;
                curRecordResults.errors.push_back(msg);
                curRecordResults.valid = false;
                if (crashOnMultikeyValidateFailure.shouldFail()) {
                    invariant(false, msg);
                }
            }
        }

        if (index->isMultikey(opCtx, coll)) {
            const MultikeyPaths& indexPaths = index->getMultikeyPaths(opCtx, coll);
            if (!MultikeyPathTracker::covers(indexPaths, *documentMultikeyPaths.get())) {
                if (_validateState->fixErrors()) {
                    writeConflictRetry(opCtx, "increaseMultikeyPathCoverage", coll->ns().ns(), [&] {
                        WriteUnitOfWork wuow(opCtx);
                        coll->getIndexCatalog()->setMultikeyPaths(
                            opCtx, coll, descriptor, *multikeyMetadataKeys, *documentMultikeyPaths);
                        wuow.commit();
                    });

                    LOGV2(4614701,
                          "Multikey paths updated to cover multikey document",
                          "indexName"_attr = descriptor->indexName(),
                          "collection"_attr = coll->ns().ns());
                    results->warnings.push_back(str::stream() << "Index " << descriptor->indexName()
                                                              << " multikey paths updated.");
                    results->repaired = true;
                } else {
                    std::string msg = str::stream()
                        << "Index " << descriptor->indexName()
                        << " multikey paths do not cover a document. RecordId: " << recordId;
                    auto& curRecordResults = (results->indexResultsMap)[descriptor->indexName()];
                    curRecordResults.errors.push_back(msg);
                    curRecordResults.valid = false;
                }
            }
        }

        IndexInfo& indexInfo = _indexConsistency->getIndexInfo(descriptor->indexName());
        if (shouldBeMultikey) {
            indexInfo.multikeyDocs = true;
        }

        // An empty set of multikey paths indicates that this index does not track path-level
        // multikey information and we should do no tracking.
        if (shouldBeMultikey && documentMultikeyPaths->size()) {
            _indexConsistency->addDocumentMultikeyPaths(&indexInfo, *documentMultikeyPaths);
        }

        for (const auto& keyString : *multikeyMetadataKeys) {
            try {
                _indexConsistency->addMultikeyMetadataPath(keyString, &indexInfo);
            } catch (...) {
                return exceptionToStatus();
            }
        }

        for (const auto& keyString : *documentKeySet) {
            try {
                _totalIndexKeys++;
                _indexConsistency->addDocKey(opCtx, keyString, &indexInfo, recordId);
            } catch (...) {
                return exceptionToStatus();
            }
        }
    }
    return status;
}

namespace {
// Ensures that index entries are in increasing or decreasing order.
void _validateKeyOrder(OperationContext* opCtx,
                       const IndexCatalogEntry* index,
                       const KeyString::Value& currKey,
                       const KeyString::Value& prevKey,
                       IndexValidateResults* results) {
    auto descriptor = index->descriptor();
    bool unique = descriptor->unique();

    // KeyStrings will be in strictly increasing order because all keys are sorted and they are in
    // the format (Key, RID), and all RecordIDs are unique.
    if (currKey.compare(prevKey) <= 0) {
        if (results && results->valid) {
            results->errors.push_back(str::stream()
                                      << "index '" << descriptor->indexName()
                                      << "' is not in strictly ascending or descending order");
        }
        if (results) {
            results->valid = false;
        }
        return;
    }

    if (unique) {
        // Unique indexes must not have duplicate keys.
        int cmp = currKey.compareWithoutRecordIdLong(prevKey);
        if (cmp != 0) {
            return;
        }

        if (results && results->valid) {
            auto bsonKey = KeyString::toBson(currKey, Ordering::make(descriptor->keyPattern()));
            auto firstRecordId =
                KeyString::decodeRecordIdLongAtEnd(prevKey.getBuffer(), prevKey.getSize());
            auto secondRecordId =
                KeyString::decodeRecordIdLongAtEnd(currKey.getBuffer(), currKey.getSize());
            results->errors.push_back(str::stream() << "Unique index '" << descriptor->indexName()
                                                    << "' has duplicate key: " << bsonKey
                                                    << ", first record: " << firstRecordId
                                                    << ", second record: " << secondRecordId);
        }
        if (results) {
            results->valid = false;
        }
    }
}
}  // namespace

void ValidateAdaptor::traverseIndex(OperationContext* opCtx,
                                    const IndexCatalogEntry* index,
                                    int64_t* numTraversedKeys,
                                    ValidateResults* results) {
    const IndexDescriptor* descriptor = index->descriptor();
    auto indexName = descriptor->indexName();
    auto& indexResults = results->indexResultsMap[indexName];
    IndexInfo& indexInfo = _indexConsistency->getIndexInfo(indexName);
    int64_t numKeys = 0;

    bool isFirstEntry = true;

    // The progress meter will be inactive after traversing the record store to allow the message
    // and the total to be set to different values.
    if (!_progress->isActive()) {
        const char* curopMessage = "Validate: scanning index entries";
        stdx::unique_lock<Client> lk(*opCtx->getClient());
        _progress.set(CurOp::get(opCtx)->setProgress_inlock(curopMessage, _totalIndexKeys));
    }

    const KeyString::Version version =
        index->accessMethod()->getSortedDataInterface()->getKeyStringVersion();

    KeyString::Builder firstKeyStringBuilder(
        version, BSONObj(), indexInfo.ord, KeyString::Discriminator::kExclusiveBefore);
    KeyString::Value firstKeyString = firstKeyStringBuilder.getValueCopy();
    KeyString::Value prevIndexKeyStringValue;

    // Ensure that this index has an open index cursor.
    const auto indexCursorIt = _validateState->getIndexCursors().find(indexName);
    invariant(indexCursorIt != _validateState->getIndexCursors().end());

    const std::unique_ptr<SortedDataInterfaceThrottleCursor>& indexCursor = indexCursorIt->second;

    boost::optional<KeyStringEntry> indexEntry;
    try {
        indexEntry = indexCursor->seekForKeyString(opCtx, firstKeyString);
    } catch (const DBException& ex) {
        if (TestingProctor::instance().isEnabled() && ex.code() != ErrorCodes::WriteConflict) {
            LOGV2_FATAL(5318400,
                        "Error seeking to first key",
                        "error"_attr = ex.toString(),
                        "index"_attr = indexName,
                        "key"_attr = firstKeyString.toString());
        }
        throw;
    }

    const auto keyFormat = index->accessMethod()->getSortedDataInterface()->rsKeyFormat();
    const RecordId kWildcardMultikeyMetadataRecordId = record_id_helpers::reservedIdFor(
        record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, keyFormat);
    while (indexEntry) {
        if (!isFirstEntry) {
            _validateKeyOrder(
                opCtx, index, indexEntry->keyString, prevIndexKeyStringValue, &indexResults);
        }

        bool isMetadataKey = indexEntry->loc == kWildcardMultikeyMetadataRecordId;
        if (descriptor->getIndexType() == IndexType::INDEX_WILDCARD && isMetadataKey) {
            _indexConsistency->removeMultikeyMetadataPath(indexEntry->keyString, &indexInfo);
        } else {
            try {
                _indexConsistency->addIndexKey(
                    opCtx, indexEntry->keyString, &indexInfo, indexEntry->loc, results);
            } catch (const DBException& e) {
                StringBuilder ss;
                ss << "Parsing index key for " << indexInfo.indexName << " recId "
                   << indexEntry->loc << " threw exception " << e.toString();
                results->errors.push_back(ss.str());
                results->valid = false;
            }
        }

        _progress->hit();
        numKeys++;
        isFirstEntry = false;
        prevIndexKeyStringValue = indexEntry->keyString;

        if (numKeys % kInterruptIntervalNumRecords == 0) {
            // Periodically checks for interrupts and yields.
            opCtx->checkForInterrupt();
            _validateState->yield(opCtx);
        }

        try {
            indexEntry = indexCursor->nextKeyString(opCtx);
        } catch (const DBException& ex) {
            if (TestingProctor::instance().isEnabled() && ex.code() != ErrorCodes::WriteConflict) {
                LOGV2_FATAL(5318401,
                            "Error advancing index cursor",
                            "error"_attr = ex.toString(),
                            "index"_attr = indexName,
                            "prevKey"_attr = prevIndexKeyStringValue.toString());
            }
            throw;
        }
    }

    if (results && _indexConsistency->getMultikeyMetadataPathCount(&indexInfo) > 0) {
        results->errors.push_back(str::stream()
                                  << "Index '" << descriptor->indexName()
                                  << "' has one or more missing multikey metadata index keys");
        results->valid = false;
    }

    // Adjust multikey metadata when allowed. These states are all allowed by the design of
    // multikey. A collection should still be valid without these adjustments.
    if (_validateState->adjustMultikey()) {

        // If this collection has documents that make this index multikey, then check whether those
        // multikey paths match the index's metadata.
        auto indexPaths = index->getMultikeyPaths(opCtx, _validateState->getCollection());
        auto& documentPaths = indexInfo.docMultikeyPaths;
        if (indexInfo.multikeyDocs && documentPaths != indexPaths) {
            LOGV2(5367500,
                  "Index's multikey paths do not match those of its documents",
                  "index"_attr = descriptor->indexName(),
                  "indexPaths"_attr = MultikeyPathTracker::dumpMultikeyPaths(indexPaths),
                  "documentPaths"_attr = MultikeyPathTracker::dumpMultikeyPaths(documentPaths));

            // Since we have the correct multikey path information for this index, we can tighten up
            // its metadata to improve query performance. This may apply in two distinct scenarios:
            // 1. Collection data has changed such that the current multikey paths on the index
            // are too permissive and certain document paths are no longer multikey.
            // 2. This index was built before 3.4, and there is no multikey path information for
            // the index. We can effectively 'upgrade' the index so that it does not need to be
            // rebuilt to update this information.
            writeConflictRetry(opCtx, "updateMultikeyPaths", _validateState->nss().ns(), [&]() {
                WriteUnitOfWork wuow(opCtx);
                auto writeableIndex = const_cast<IndexCatalogEntry*>(index);
                const bool isMultikey = true;
                writeableIndex->forceSetMultikey(
                    opCtx, _validateState->getCollection(), isMultikey, documentPaths);
                wuow.commit();
            });

            if (results) {
                results->warnings.push_back(str::stream() << "Updated index multikey metadata"
                                                          << ": " << descriptor->indexName());
                results->repaired = true;
            }
        }

        // If this index does not need to be multikey, then unset the flag.
        if (index->isMultikey(opCtx, _validateState->getCollection()) && !indexInfo.multikeyDocs) {
            invariant(!indexInfo.docMultikeyPaths.size());

            LOGV2(5367501,
                  "Index is multikey but there are no multikey documents",
                  "index"_attr = descriptor->indexName());

            // This makes an improvement in the case that no documents make the index multikey and
            // the flag can be unset entirely. This may be due to a change in the data or historical
            // multikey bugs that have persisted incorrect multikey infomation.
            writeConflictRetry(opCtx, "unsetMultikeyPaths", _validateState->nss().ns(), [&]() {
                WriteUnitOfWork wuow(opCtx);
                auto writeableIndex = const_cast<IndexCatalogEntry*>(index);
                const bool isMultikey = false;
                writeableIndex->forceSetMultikey(
                    opCtx, _validateState->getCollection(), isMultikey, {});
                wuow.commit();
            });

            if (results) {
                results->warnings.push_back(str::stream() << "Unset index multikey metadata"
                                                          << ": " << descriptor->indexName());
                results->repaired = true;
            }
        }
    }

    if (numTraversedKeys) {
        *numTraversedKeys = numKeys;
    }
}

void ValidateAdaptor::traverseRecordStore(OperationContext* opCtx,
                                          ValidateResults* results,
                                          BSONObjBuilder* output) {
    _numRecords = 0;  // need to reset it because this function can be called more than once.
    long long dataSizeTotal = 0;
    long long interruptIntervalNumBytes = 0;
    long long nInvalid = 0;
    long long numCorruptRecordsSizeBytes = 0;

    ON_BLOCK_EXIT([&]() {
        output->appendNumber("nInvalidDocuments", nInvalid);
        output->appendNumber("nrecords", _numRecords);
        _progress->finished();
    });

    RecordId prevRecordId;

    // In case validation occurs twice and the progress meter persists after index traversal
    if (_progress.get() && _progress->isActive()) {
        _progress->finished();
    }

    // Because the progress meter is intended as an approximation, it's sufficient to get the number
    // of records when we begin traversing, even if this number may deviate from the final number.
    const char* curopMessage = "Validate: scanning documents";
    const auto totalRecords = _validateState->getCollection()->getRecordStore()->numRecords(opCtx);
    const auto rs = _validateState->getCollection()->getRecordStore();
    {
        stdx::unique_lock<Client> lk(*opCtx->getClient());
        _progress.set(CurOp::get(opCtx)->setProgress_inlock(curopMessage, totalRecords));
    }

    if (_validateState->getFirstRecordId().isNull()) {
        // The record store is empty if the first RecordId isn't initialized.
        return;
    }

    bool corruptRecordsSizeLimitWarning = false;
    const std::unique_ptr<SeekableRecordThrottleCursor>& traverseRecordStoreCursor =
        _validateState->getTraverseRecordStoreCursor();
    for (auto record =
             traverseRecordStoreCursor->seekExact(opCtx, _validateState->getFirstRecordId());
         record;
         record = traverseRecordStoreCursor->next(opCtx)) {
        _progress->hit();
        ++_numRecords;
        auto dataSize = record->data.size();
        interruptIntervalNumBytes += dataSize;
        dataSizeTotal += dataSize;
        size_t validatedSize = 0;
        Status status = validateRecord(opCtx, record->id, record->data, &validatedSize, results);

        // RecordStores are required to return records in RecordId order.
        if (prevRecordId.isValid()) {
            invariant(prevRecordId < record->id);
        }

        // validatedSize = dataSize is not a general requirement as some storage engines may use
        // padding, but we still require that they return the unpadded record data.
        if (!status.isOK() || validatedSize != static_cast<size_t>(dataSize)) {
            // If status is not okay, dataSize is not reliable.
            if (!status.isOK()) {
                LOGV2(4835001,
                      "Document corruption details - Document validation failed with error",
                      "recordId"_attr = record->id,
                      "error"_attr = status);
            } else {
                LOGV2(4835002,
                      "Document corruption details - Document validation failure; size mismatch",
                      "recordId"_attr = record->id,
                      "validatedBytes"_attr = validatedSize,
                      "recordBytes"_attr = dataSize);
            }

            if (_validateState->fixErrors()) {
                writeConflictRetry(
                    opCtx, "corrupt record removal", _validateState->nss().ns(), [&] {
                        WriteUnitOfWork wunit(opCtx);
                        rs->deleteRecord(opCtx, record->id);
                        wunit.commit();
                    });
                results->repaired = true;
                results->numRemovedCorruptRecords++;
                _numRecords--;
            } else {
                if (results->valid) {
                    results->errors.push_back("Detected one or more invalid documents. See logs.");
                    results->valid = false;
                }

                numCorruptRecordsSizeBytes += record->id.memUsage();
                if (numCorruptRecordsSizeBytes <= kMaxErrorSizeBytes) {
                    results->corruptRecords.push_back(record->id);
                } else if (!corruptRecordsSizeLimitWarning) {
                    results->warnings.push_back(
                        "Not all corrupted records are listed due to size limitations.");
                    corruptRecordsSizeLimitWarning = true;
                }

                nInvalid++;
            }
        } else {
            // If the document is not corrupted, validate the document against this collection's
            // schema validator. Don't treat invalid documents as errors since documents can bypass
            // document validation when being inserted or updated.
            status = _validateState->getCollection()->checkValidation(opCtx, record->data.toBson());
            if (!status.isOK()) {
                LOGV2_WARNING(5363500,
                              "Document is not compliant with the collection's schema",
                              logAttrs(_validateState->getCollection()->ns()),
                              "recordId"_attr = record->id,
                              "reason"_attr = status);

                if (!_validateState->isCollectionSchemaViolated()) {
                    _validateState->setCollectionSchemaViolated();
                    results->warnings.push_back(
                        "Detected one or more documents not compliant with the collection's "
                        "schema. See logs.");
                }
            }
        }

        prevRecordId = record->id;

        if (_numRecords % kInterruptIntervalNumRecords == 0 ||
            interruptIntervalNumBytes >= kInterruptIntervalNumBytes) {
            // Periodically checks for interrupts and yields.
            opCtx->checkForInterrupt();
            _validateState->yield(opCtx);

            if (interruptIntervalNumBytes >= kInterruptIntervalNumBytes) {
                interruptIntervalNumBytes = 0;
            }
        }
    }

    if (results->numRemovedCorruptRecords > 0) {
        results->warnings.push_back(str::stream() << "Removed " << results->numRemovedCorruptRecords
                                                  << " invalid documents.");
    }

    const auto fastCount = _validateState->getCollection()->numRecords(opCtx);
    if (_validateState->shouldEnforceFastCount() && fastCount != _numRecords) {
        results->errors.push_back(str::stream() << "fast count (" << fastCount
                                                << ") does not match number of records ("
                                                << _numRecords << ") for collection '"
                                                << _validateState->getCollection()->ns() << "'");
        results->valid = false;
    }

    // Do not update the record store stats if we're in the background as we've validated a
    // checkpoint and it may not have the most up-to-date changes.
    if (results->valid && !_validateState->isBackground()) {
        _validateState->getCollection()->getRecordStore()->updateStatsAfterRepair(
            opCtx, _numRecords, dataSizeTotal);
    }
}

void ValidateAdaptor::validateIndexKeyCount(OperationContext* opCtx,
                                            const IndexCatalogEntry* index,
                                            IndexValidateResults& results) {
    // Fetch the total number of index entries we previously found traversing the index.
    const IndexDescriptor* desc = index->descriptor();
    const std::string indexName = desc->indexName();
    IndexInfo* indexInfo = &_indexConsistency->getIndexInfo(indexName);
    auto numTotalKeys = indexInfo->numKeys;

    // Do not fail on finding too few index entries compared to collection entries when full:false.
    bool hasTooFewKeys = false;
    bool noErrorOnTooFewKeys = !_validateState->isFullIndexValidation();

    if (desc->isIdIndex() && numTotalKeys != _numRecords) {
        hasTooFewKeys = (numTotalKeys < _numRecords);
        std::string msg = str::stream()
            << "number of _id index entries (" << numTotalKeys
            << ") does not match the number of documents in the index (" << _numRecords << ")";
        if (noErrorOnTooFewKeys && (numTotalKeys < _numRecords)) {
            results.warnings.push_back(msg);
        } else {
            results.errors.push_back(msg);
            results.valid = false;
        }
    }

    // Hashed indexes may never be multikey.
    if (desc->getAccessMethodName() == IndexNames::HASHED &&
        index->isMultikey(opCtx, _validateState->getCollection())) {
        results.errors.push_back(str::stream() << "Hashed index is incorrectly marked multikey: "
                                               << desc->indexName());
        results.valid = false;
    }

    // Confirm that the number of index entries is not greater than the number of documents in the
    // collection. This check is only valid for indexes that are not multikey (indexed arrays
    // produce an index key per array entry) and not $** indexes which can produce index keys for
    // multiple paths within a single document.
    if (results.valid && !index->isMultikey(opCtx, _validateState->getCollection()) &&
        desc->getIndexType() != IndexType::INDEX_WILDCARD && numTotalKeys > _numRecords) {
        std::string err = str::stream()
            << "index " << desc->indexName() << " is not multi-key, but has more entries ("
            << numTotalKeys << ") than documents in the index (" << _numRecords << ")";
        results.errors.push_back(err);
        results.valid = false;
    }

    // Ignore any indexes with a special access method. If an access method name is given, the
    // index may be a full text, geo or special index plugin with different semantics.
    if (results.valid && !desc->isSparse() && !desc->isPartial() && !desc->isIdIndex() &&
        desc->getAccessMethodName() == "" && numTotalKeys < _numRecords) {
        hasTooFewKeys = true;
        std::string msg = str::stream()
            << "index " << desc->indexName() << " is not sparse or partial, but has fewer entries ("
            << numTotalKeys << ") than documents in the index (" << _numRecords << ")";
        if (noErrorOnTooFewKeys) {
            results.warnings.push_back(msg);
        } else {
            results.errors.push_back(msg);
            results.valid = false;
        }
    }

    if (!_validateState->isFullIndexValidation() && hasTooFewKeys) {
        std::string warning = str::stream()
            << "index " << desc->indexName() << " has fewer keys than records."
            << " Please re-run the validate command with {full: true}";
        results.warnings.push_back(warning);
    }
}
}  // namespace mongo
