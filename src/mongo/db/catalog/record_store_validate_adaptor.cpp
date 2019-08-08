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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/record_store_validate_adaptor.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_consistency.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/wildcard_access_method.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/rpc/object_check.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {
KeyString::Builder makeWildCardMultikeyMetadataKeyString(const BSONObj& indexKey) {
    const auto multikeyMetadataOrd = Ordering::make(BSON("" << 1 << "" << 1));
    const RecordId multikeyMetadataRecordId(RecordId::ReservedId::kWildcardMultikeyMetadataId);
    return {KeyString::Version::kLatestVersion,
            indexKey,
            multikeyMetadataOrd,
            multikeyMetadataRecordId};
}
}  // namespace

Status RecordStoreValidateAdaptor::validate(const RecordId& recordId,
                                            const RecordData& record,
                                            size_t* dataSize) {
    BSONObj recordBson;
    try {
        recordBson = record.toBson();
    } catch (...) {
        return exceptionToStatus();
    }

    const Status status = validateBSON(
        recordBson.objdata(), recordBson.objsize(), Validator<BSONObj>::enabledBSONVersion());
    if (status.isOK()) {
        *dataSize = recordBson.objsize();
    } else {
        return status;
    }

    if (!_indexCatalog->haveAnyIndexes()) {
        return status;
    }

    for (auto& it : _indexConsistency->getIndexInfo()) {
        IndexInfo& indexInfo = it.second;
        const IndexDescriptor* descriptor = indexInfo.descriptor;
        const IndexAccessMethod* iam = _indexCatalog->getEntry(descriptor)->accessMethod();

        if (descriptor->isPartial()) {
            const IndexCatalogEntry* ice = _indexCatalog->getEntry(descriptor);
            if (!ice->getFilterExpression()->matchesBSON(recordBson)) {
                continue;
            }
        }

        KeyStringSet documentKeySet;
        KeyStringSet multikeyMetadataKeys;
        MultikeyPaths multikeyPaths;
        iam->getKeys(recordBson,
                     IndexAccessMethod::GetKeysMode::kEnforceConstraints,
                     &documentKeySet,
                     &multikeyMetadataKeys,
                     &multikeyPaths,
                     recordId);

        if (!descriptor->isMultikey(_opCtx) &&
            iam->shouldMarkIndexAsMultikey(
                {documentKeySet.begin(), documentKeySet.end()},
                {multikeyMetadataKeys.begin(), multikeyMetadataKeys.end()},
                multikeyPaths)) {
            std::string msg = str::stream()
                << "Index " << descriptor->indexName() << " is not multi-key but has more than one"
                << " key in document " << recordId;
            ValidateResults& curRecordResults = (*_indexNsResultsMap)[descriptor->indexName()];
            curRecordResults.errors.push_back(msg);
            curRecordResults.valid = false;
        }

        for (const auto& keyString : multikeyMetadataKeys) {
            try {
                auto key = KeyString::toBsonSafe(keyString.getBuffer(),
                                                 keyString.getSize(),
                                                 indexInfo.ord,
                                                 keyString.getTypeBits());
                _indexConsistency->addMultikeyMetadataPath(
                    makeWildCardMultikeyMetadataKeyString(key), &indexInfo);
            } catch (...) {
                return exceptionToStatus();
            }
        }

        for (const auto& keyString : documentKeySet) {
            try {
                auto key = KeyString::toBsonSafe(keyString.getBuffer(),
                                                 keyString.getSize(),
                                                 indexInfo.ord,
                                                 keyString.getTypeBits());
                indexInfo.ks->resetToKey(key, indexInfo.ord, recordId);
                _indexConsistency->addDocKey(*indexInfo.ks, &indexInfo, recordId, key);
            } catch (...) {
                return exceptionToStatus();
            }
        }
    }
    return status;
}

void RecordStoreValidateAdaptor::traverseIndex(const IndexAccessMethod* iam,
                                               const IndexDescriptor* descriptor,
                                               ValidateResults* results,
                                               int64_t* numTraversedKeys) {
    auto indexName = descriptor->indexName();
    IndexInfo* indexInfo = &_indexConsistency->getIndexInfo(indexName);
    int64_t numKeys = 0;

    const auto& key = descriptor->keyPattern();
    const Ordering ord = Ordering::make(key);
    bool isFirstEntry = true;

    std::unique_ptr<SortedDataInterface::Cursor> cursor = iam->newCursor(_opCtx, true);
    // We want to use the latest version of KeyString here.
    const KeyString::Version version = KeyString::Version::kLatestVersion;
    std::unique_ptr<KeyString::Builder> indexKeyStringBuilder =
        std::make_unique<KeyString::Builder>(version);
    std::unique_ptr<KeyString::Builder> prevIndexKeyStringBuilder =
        std::make_unique<KeyString::Builder>(version);

    // Seeking to BSONObj() is equivalent to seeking to the first entry of an index.
    for (auto indexEntry = cursor->seek(BSONObj(), true); indexEntry; indexEntry = cursor->next()) {
        indexKeyStringBuilder->resetToKey(indexEntry->key, ord, indexEntry->loc);

        // Ensure that the index entries are in increasing or decreasing order.
        if (!isFirstEntry && *indexKeyStringBuilder < *prevIndexKeyStringBuilder) {
            if (results && results->valid) {
                results->errors.push_back(
                    "one or more indexes are not in strictly ascending or descending order");
            }

            if (results) {
                results->valid = false;
            }
        }

        const RecordId kWildcardMultikeyMetadataRecordId{
            RecordId::ReservedId::kWildcardMultikeyMetadataId};
        if (descriptor->getIndexType() == IndexType::INDEX_WILDCARD &&
            indexEntry->loc == kWildcardMultikeyMetadataRecordId) {
            _indexConsistency->removeMultikeyMetadataPath(
                makeWildCardMultikeyMetadataKeyString(indexEntry->key), indexInfo);
            numKeys++;
            continue;
        }

        _indexConsistency->addIndexKey(
            *indexKeyStringBuilder, indexInfo, indexEntry->loc, indexEntry->key);

        numKeys++;
        isFirstEntry = false;
        prevIndexKeyStringBuilder.swap(indexKeyStringBuilder);
    }

    if (results && _indexConsistency->getMultikeyMetadataPathCount(indexInfo) > 0) {
        results->errors.push_back(str::stream()
                                  << "Index '" << descriptor->indexName()
                                  << "' has one or more missing multikey metadata index keys");
        results->valid = false;
    }

    if (numTraversedKeys) {
        *numTraversedKeys = numKeys;
    }
}

void RecordStoreValidateAdaptor::traverseRecordStore(RecordStore* recordStore,
                                                     ValidateResults* results,
                                                     BSONObjBuilder* output) {
    long long nrecords = 0;
    long long dataSizeTotal = 0;
    long long nInvalid = 0;

    results->valid = true;
    std::unique_ptr<SeekableRecordCursor> cursor = recordStore->getCursor(_opCtx, true);
    int interruptInterval = 4096;
    RecordId prevRecordId;

    while (auto record = cursor->next()) {
        ++nrecords;

        if (!(nrecords % interruptInterval)) {
            _opCtx->checkForInterrupt();
        }

        auto dataSize = record->data.size();
        dataSizeTotal += dataSize;
        size_t validatedSize;
        Status status = validate(record->id, record->data, &validatedSize);

        // Checks to ensure isInRecordIdOrder() is being used properly.
        if (prevRecordId.isValid()) {
            invariant(prevRecordId < record->id);
        }

        // While some storage engines may use padding, we still require that they return the
        // unpadded record data.
        if (!status.isOK() || validatedSize != static_cast<size_t>(dataSize)) {
            if (results->valid) {
                // Only log once.
                results->errors.push_back("detected one or more invalid documents (see logs)");
            }
            nInvalid++;
            results->valid = false;
            log() << "document at location: " << record->id << " is corrupted";
        }

        prevRecordId = record->id;
    }

    if (results->valid) {
        recordStore->updateStatsAfterRepair(_opCtx, nrecords, dataSizeTotal);
    }

    output->append("nInvalidDocuments", nInvalid);
    output->appendNumber("nrecords", nrecords);
}

void RecordStoreValidateAdaptor::validateIndexKeyCount(const IndexDescriptor* idx,
                                                       int64_t numRecs,
                                                       ValidateResults& results) {
    const std::string indexName = idx->indexName();
    IndexInfo* indexInfo = &_indexConsistency->getIndexInfo(indexName);
    auto numTotalKeys = indexInfo->numKeys;

    bool hasTooFewKeys = false;
    bool noErrorOnTooFewKeys = (_level != kValidateFull);

    if (idx->isIdIndex() && numTotalKeys != numRecs) {
        hasTooFewKeys = numTotalKeys < numRecs ? true : hasTooFewKeys;
        std::string msg = str::stream()
            << "number of _id index entries (" << numTotalKeys
            << ") does not match the number of documents in the index (" << numRecs << ")";
        if (noErrorOnTooFewKeys && (numTotalKeys < numRecs)) {
            results.warnings.push_back(msg);
        } else {
            results.errors.push_back(msg);
            results.valid = false;
        }
    }

    // Confirm that the number of index entries is not greater than the number of documents in the
    // collection. This check is only valid for indexes that are not multikey (indexed arrays
    // produce an index key per array entry) and not $** indexes which can produce index keys for
    // multiple paths within a single document.
    if (results.valid && !idx->isMultikey(_opCtx) &&
        idx->getIndexType() != IndexType::INDEX_WILDCARD && numTotalKeys > numRecs) {
        std::string err = str::stream()
            << "index " << idx->indexName() << " is not multi-key, but has more entries ("
            << numTotalKeys << ") than documents in the index (" << numRecs << ")";
        results.errors.push_back(err);
        results.valid = false;
    }
    // Ignore any indexes with a special access method. If an access method name is given, the
    // index may be a full text, geo or special index plugin with different semantics.
    if (results.valid && !idx->isSparse() && !idx->isPartial() && !idx->isIdIndex() &&
        idx->getAccessMethodName() == "" && numTotalKeys < numRecs) {
        hasTooFewKeys = true;
        std::string msg = str::stream()
            << "index " << idx->indexName() << " is not sparse or partial, but has fewer entries ("
            << numTotalKeys << ") than documents in the index (" << numRecs << ")";
        if (noErrorOnTooFewKeys) {
            results.warnings.push_back(msg);
        } else {
            results.errors.push_back(msg);
            results.valid = false;
        }
    }

    if ((_level != kValidateFull) && hasTooFewKeys) {
        std::string warning = str::stream()
            << "index " << idx->indexName() << " has fewer keys than records."
            << " Please re-run the validate command with {full: true}";
        results.warnings.push_back(warning);
    }
}
}  // namespace mongo
