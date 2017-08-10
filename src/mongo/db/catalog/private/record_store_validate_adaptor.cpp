/*-
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/private/record_store_validate_adaptor.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_consistency.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/rpc/object_check.h"
#include "mongo/util/log.h"

namespace mongo {

Status RecordStoreValidateAdaptor::validate(const RecordId& recordId,
                                            const RecordData& record,
                                            size_t* dataSize) {
    BSONObj recordBson = record.toBson();

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

    IndexCatalog::IndexIterator i = _indexCatalog->getIndexIterator(_opCtx, false);

    while (i.more()) {
        const IndexDescriptor* descriptor = i.next();
        const std::string indexNs = descriptor->indexNamespace();
        int indexNumber = _indexConsistency->getIndexNumber(indexNs);
        ValidateResults curRecordResults;

        const IndexAccessMethod* iam = _indexCatalog->getIndex(descriptor);

        if (descriptor->isPartial()) {
            const IndexCatalogEntry* ice = _indexCatalog->getEntry(descriptor);
            if (!ice->getFilterExpression()->matchesBSON(recordBson)) {
                (*_indexNsResultsMap)[indexNs] = curRecordResults;
                continue;
            }
        }

        BSONObjSet documentKeySet = SimpleBSONObjComparator::kInstance.makeBSONObjSet();
        // There's no need to compute the prefixes of the indexed fields that cause the
        // index to be multikey when validating the index keys.
        MultikeyPaths* multikeyPaths = nullptr;
        iam->getKeys(recordBson,
                     IndexAccessMethod::GetKeysMode::kEnforceConstraints,
                     &documentKeySet,
                     multikeyPaths);

        if (!descriptor->isMultikey(_opCtx) && documentKeySet.size() > 1) {
            std::string msg = str::stream() << "Index " << descriptor->indexName()
                                            << " is not multi-key but has more than one"
                                            << " key in document " << recordId;
            curRecordResults.errors.push_back(msg);
            curRecordResults.valid = false;
        }

        const auto& pattern = descriptor->keyPattern();
        const Ordering ord = Ordering::make(pattern);

        for (const auto& key : documentKeySet) {
            if (key.objsize() >= static_cast<int64_t>(KeyString::TypeBits::kMaxKeyBytes)) {
                // Index keys >= 1024 bytes are not indexed.
                _indexConsistency->addLongIndexKey(indexNumber);
                continue;
            }

            // We want to use the latest version of KeyString here.
            KeyString ks(KeyString::kLatestVersion, key, ord, recordId);
            _indexConsistency->addDocKey(ks, indexNumber);
        }
        (*_indexNsResultsMap)[indexNs] = curRecordResults;
    }
    return status;
}

void RecordStoreValidateAdaptor::traverseIndex(const IndexAccessMethod* iam,
                                               const IndexDescriptor* descriptor,
                                               ValidateResults* results,
                                               int64_t* numTraversedKeys) {
    auto indexNs = descriptor->indexNamespace();
    int indexNumber = _indexConsistency->getIndexNumber(indexNs);
    int64_t numKeys = 0;

    const auto& key = descriptor->keyPattern();
    const Ordering ord = Ordering::make(key);
    KeyString::Version version = KeyString::kLatestVersion;
    std::unique_ptr<KeyString> prevIndexKeyString = nullptr;
    bool isFirstEntry = true;

    std::unique_ptr<SortedDataInterface::Cursor> cursor = iam->newCursor(_opCtx, true);
    // Seeking to BSONObj() is equivalent to seeking to the first entry of an index.
    for (auto indexEntry = cursor->seek(BSONObj(), true); indexEntry; indexEntry = cursor->next()) {

        // We want to use the latest version of KeyString here.
        std::unique_ptr<KeyString> indexKeyString =
            stdx::make_unique<KeyString>(version, indexEntry->key, ord, indexEntry->loc);
        // Ensure that the index entries are in increasing or decreasing order.
        if (!isFirstEntry && *indexKeyString < *prevIndexKeyString) {
            if (results->valid) {
                results->errors.push_back(
                    "one or more indexes are not in strictly ascending or descending "
                    "order");
            }
            results->valid = false;
        }

        _indexConsistency->addIndexKey(*indexKeyString, indexNumber);

        numKeys++;
        isFirstEntry = false;
        prevIndexKeyString.swap(indexKeyString);
    }

    *numTraversedKeys = numKeys;
}

void RecordStoreValidateAdaptor::traverseRecordStore(RecordStore* recordStore,
                                                     ValidateCmdLevel level,
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
        if (prevRecordId.isNormal()) {
            invariant(prevRecordId < record->id);
        }

        // While some storage engines, such as MMAPv1, may use padding, we still require
        // that they return the unpadded record data.
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

void RecordStoreValidateAdaptor::validateIndexKeyCount(IndexDescriptor* idx,
                                                       int64_t numRecs,
                                                       ValidateResults& results) {
    const std::string indexNs = idx->indexNamespace();
    int indexNumber = _indexConsistency->getIndexNumber(indexNs);
    int64_t numIndexedKeys = _indexConsistency->getNumKeys(indexNumber);
    int64_t numLongKeys = _indexConsistency->getNumLongKeys(indexNumber);
    auto totalKeys = numLongKeys + numIndexedKeys;

    bool hasTooFewKeys = false;
    bool noErrorOnTooFewKeys = !failIndexKeyTooLong.load() && (_level != kValidateFull);

    if (idx->isIdIndex() && totalKeys != numRecs) {
        hasTooFewKeys = totalKeys < numRecs ? true : hasTooFewKeys;
        std::string msg = str::stream() << "number of _id index entries (" << numIndexedKeys
                                        << ") does not match the number of documents in the index ("
                                        << numRecs - numLongKeys << ")";
        if (noErrorOnTooFewKeys && (numIndexedKeys < numRecs)) {
            results.warnings.push_back(msg);
        } else {
            results.errors.push_back(msg);
            results.valid = false;
        }
    }

    if (results.valid && !idx->isMultikey(_opCtx) && totalKeys > numRecs) {
        std::string err = str::stream()
            << "index " << idx->indexName() << " is not multi-key, but has more entries ("
            << numIndexedKeys << ") than documents in the index (" << numRecs - numLongKeys << ")";
        results.errors.push_back(err);
        results.valid = false;
    }
    // Ignore any indexes with a special access method. If an access method name is given, the
    // index may be a full text, geo or special index plugin with different semantics.
    if (results.valid && !idx->isSparse() && !idx->isPartial() && !idx->isIdIndex() &&
        idx->getAccessMethodName() == "" && totalKeys < numRecs) {
        hasTooFewKeys = true;
        std::string msg = str::stream()
            << "index " << idx->indexName() << " is not sparse or partial, but has fewer entries ("
            << numIndexedKeys << ") than documents in the index (" << numRecs - numLongKeys << ")";
        if (noErrorOnTooFewKeys) {
            results.warnings.push_back(msg);
        } else {
            results.errors.push_back(msg);
            results.valid = false;
        }
    }

    if ((_level != kValidateFull) && hasTooFewKeys) {
        std::string warning = str::stream()
            << "index " << idx->indexName()
            << " has fewer keys than records. This may be the result of currently or "
               "previously running the server with the failIndexKeyTooLong parameter set to "
               "false. Please re-run the validate command with {full: true}";
        results.warnings.push_back(warning);
    }
}
}  // namespace
