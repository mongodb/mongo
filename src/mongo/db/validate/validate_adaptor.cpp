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


#include "mongo/db/validate/validate_adaptor.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/index_names.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_options_gen.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_impl.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/mdb_catalog.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/throttle_cursor.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/validate/index_consistency.h"
#include "mongo/db/validate/validate_timeseries.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/object_check.h"  // IWYU pragma: keep
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/shared_buffer_fragment.h"
#include "mongo/util/str.h"
#include "mongo/util/testing_proctor.h"

#include <cstdint>
#include <memory>
#include <string>

#include <absl/container/flat_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(failRecordStoreTraversal);

// Set limit for size of corrupted records that will be reported.
const long long kMaxErrorSizeBytes = 1 * 1024 * 1024;
const long long kInterruptIntervalNumBytes = 50 * 1024 * 1024;  // 50MB.

static constexpr const char* kBSONValidationNonConformantReason =
    "Detected one or more documents in this collection not conformant to BSON specifications. For "
    "more info, see logs with log id 6825900";
static constexpr const char* kBSONValidationObjectTooLargeReason =
    "Detected one or more documents in this collection exceeding BSON object size limit. For more "
    "info, see logs with log id 10869900";
static constexpr char kOutOfOrderDocumentError[] = "Detected out-of-order documents. See logs.";
static constexpr char kInvalidDocumentError[] = "Detected one or more invalid documents. See logs.";
static constexpr char kNotEnoughSpaceToReportCorruptionWarning[] =
    "Not all corrupted records are listed due to size limitations.";

const char* _describeDocumentValidationResult(Collection::DocumentValidationResult cvr) {
    using NCR = Collection::DocumentValidationResult::NonComplianceReason;
    using SVR = Collection::SchemaValidationResult;
    // The log ID embedded in each message must match the LOGV2 call site that actually emits
    // the per-document entry: 11634800 for timeseries collections, 5363500 for all others.
    // Timeseries collections do not support user-defined validators, validationLevel, or
    // validationAction, so the only reachable timeseries reasons are kBypassProhibitedForTimeseries
    // and kTimeseriesSchemaViolation.
    switch (cvr.reason) {
        case NCR::kNone:
            return "Valid";
        case NCR::kValidatorError:
            return "Detected one or more documents not compliant with the collection's schema "
                   "because the collection's validator expression is invalid. Check logs for "
                   "log id 5363500.";
        case NCR::kBypassProhibitedWithConstraintLevel:
            return "Detected one or more documents where bypassDocumentValidation was attempted "
                   "but is not permitted with 'constraint' validationLevel. Check logs for "
                   "log id 5363500.";
        case NCR::kBypassProhibitedWithPrepareConstraintLevel:
            return "Detected one or more documents where bypassDocumentValidation was attempted "
                   "but is not permitted while prepareConstraintValidationLevel is set. Check "
                   "logs for log id 5363500.";
        case NCR::kBypassProhibitedForTimeseries:
            return "Detected one or more time-series bucket documents where "
                   "bypassDocumentValidation was attempted but is not permitted on timeseries "
                   "collections. Check logs for log id 11634800.";
        case NCR::kApiVersionIncompatible:
            return "Detected one or more documents not compliant with the collection's schema "
                   "because the validator uses expressions incompatible with the current API "
                   "version. Check logs for log id 5363500.";
        case NCR::kSchemaViolationWarnConstraintLevel:
            return "Detected one or more documents not compliant with the collection's schema "
                   "with 'warn' validation action, escalated to error because validationLevel is "
                   "'constraint'. Check logs for log id 5363500.";
        case NCR::kSchemaViolation:
            if (cvr.result == SVR::kWarn)
                return "Detected one or more documents not compliant with the collection's schema "
                       "with 'warn' validation action. Check logs for log id 5363500.";
            if (cvr.result == SVR::kErrorAndLog)
                return "Detected one or more documents not compliant with the collection's schema "
                       "with 'errorAndLog' validation action. Check logs for log id 5363500.";
            return "Detected one or more documents not compliant with the collection's schema "
                   "with 'error' validation action. Check logs for log id 5363500.";
        case NCR::kTimeseriesSchemaViolation:
            if (cvr.result == SVR::kErrorAndLog)
                return "Detected one or more time-series bucket documents not compliant with "
                       "time-series specifications with 'errorAndLog' validation action. Check "
                       "logs for log id 11634800.";
            return "Detected one or more time-series bucket documents not compliant with "
                   "time-series specifications with 'error' validation action. Check logs for "
                   "log id 11634800.";
    }
    MONGO_UNREACHABLE;
}

/**
 * Validate that for each record in a clustered RecordStore the record key (RecordId) matches the
 * document's cluster key in the record value.
 */
void _validateClusteredCollectionRecordId(OperationContext* opCtx,
                                          const RecordId& rid,
                                          const BSONObj& doc,
                                          const ClusteredIndexSpec& indexSpec,
                                          const CollatorInterface* collator,
                                          ValidateResults* results) {
    const auto ridFromDoc = record_id_helpers::keyForDoc(doc, indexSpec, collator);
    if (!ridFromDoc.isOK()) {
        results->addError(str::stream() << rid << " " << ridFromDoc.getStatus().reason());
        results->addCorruptRecord(rid);
        return;
    }

    const auto ksFromBSON =
        key_string::Builder(key_string::Version::kLatestVersion, ridFromDoc.getValue());
    const auto ksFromRid = key_string::Builder(key_string::Version::kLatestVersion, rid);

    const auto clusterKeyField = clustered_util::getClusterKeyFieldName(indexSpec);
    if (ksFromRid != ksFromBSON) {
        results->addError(str::stream()
                          << "Document with " << rid << " has mismatched " << doc[clusterKeyField]
                          << " (RecordId KeyString='" << ksFromRid.toString()
                          << "', cluster key KeyString='" << ksFromBSON.toString() << "')");
        results->addCorruptRecord(rid);
    }
}

}  // namespace

auto ValidateAdaptor::validateRecord(OperationContext* opCtx,
                                     const RecordId& recordId,
                                     const RecordData& record,
                                     long long& nNonCompliantDocuments,
                                     long long& nInvalidDocuments,
                                     ValidateResults* results,
                                     ValidationVersion validationVersion) -> ValidateRecordResult {
    {
        const Status bsonValidationStatus = validateBSON(
            record.data(), record.size(), _validateState->getBSONValidateMode(), validationVersion);

        if (!bsonValidationStatus.isOK()) {
            bool includeReason{false};
            switch (bsonValidationStatus.code()) {
                case ErrorCodes::NonConformantBSON:
                    LOGV2_WARNING_OPTIONS(6825900,
                                          {logv2::LogTruncation::Disabled},
                                          "Document is not conformant to BSON specifications",
                                          "recordId"_attr = recordId,
                                          "reason"_attr = bsonValidationStatus);
                    ++nNonCompliantDocuments;
                    results->addWarning(kBSONValidationNonConformantReason);
                    break;
                case ErrorCodes::InvalidBSONColumn:
                    // For these cases, include the reason with the validation results, the
                    // cardinality of reasons is bounded.  For other error messages, keep these
                    // separate.
                    includeReason = true;
                    [[fallthrough]];
                default:
                    LOGV2_ERROR_OPTIONS(12395400,
                                        {logv2::LogTruncation::Disabled},
                                        "Error occurred during BSON validation",
                                        "recordId"_attr = recordId,
                                        "reason"_attr = bsonValidationStatus);
                    return {.status = bsonValidationStatus,
                            .errorMessage = fmt::format(
                                "BSON validation failed with error '{}'{}. For more info, "
                                "see logs "
                                "with log id 12395400",
                                ErrorCodes::errorString(bsonValidationStatus.code()),
                                includeReason ? fmt::format(": {}", bsonValidationStatus.reason())
                                              : std::string(""))};
            }
        } else if (!_validateState->nss().isOplog()) {
            // Additionally check size if the BSON object is compliant. Do not run this check on the
            // oplog as entries are expected to exceed the max allowed user size. Use the internal
            // size for internal collections.
            const auto objSizeLimit = _validateState->nss().isOnInternalDb()
                ? BSONObjMaxInternalSize
                : BSONObjMaxUserSize;
            Status sizeValidationStatus = record.toBson().validateBSONObjSize(objSizeLimit);

            if (!sizeValidationStatus.isOK()) {
                if (sizeValidationStatus.code() == ErrorCodes::BSONObjectTooLarge) {
                    LOGV2_ERROR_OPTIONS(10869900,
                                        {logv2::LogTruncation::Disabled},
                                        "Document BSON object is too large.",
                                        "recordId"_attr = recordId,
                                        "ns"_attr = _validateState->nss(),
                                        "reason"_attr = sizeValidationStatus);
                    ++nInvalidDocuments;
                    results->addError(kBSONValidationObjectTooLargeReason,
                                      /*stopValidation=*/false);
                } else {
                    return {.status = sizeValidationStatus,
                            .errorMessage =
                                boost::none};  // Error is not related to BSON size limitations
                }
            }
        }
    }

    const BSONObj recordBson = record.toBson();

    if (MONGO_unlikely(_validateState->logDiagnostics())) {
        LOGV2(4666601, "[validate]", "recordId"_attr = recordId, "recordData"_attr = recordBson);
    }

    const CollectionPtr& coll = _validateState->getCollection();
    if (coll->isClustered()) {
        _validateClusteredCollectionRecordId(opCtx,
                                             recordId,
                                             recordBson,
                                             coll->getClusteredInfo()->getIndexSpec(),
                                             coll->getDefaultCollator(),
                                             results);
    }

    SharedBufferFragmentBuilder pool(key_string::HeapBuilder::kHeapAllocatorDefaultBytes);

    for (const auto& indexIdent : _validateState->getIndexIdents()) {
        const auto indexEntry = coll->getIndexCatalog()->findIndexByIdent(opCtx, indexIdent);
        if ((indexEntry->descriptor()->isPartial() &&
             !exec::matcher::matchesBSON(indexEntry->getFilterExpression(), recordBson)) ||
            !results->getIndexValidateResult(indexEntry->descriptor()->indexName())
                 .continueValidation()) {
            continue;
        }

        this->traverseRecord(opCtx, coll, indexEntry, recordId, recordBson, results);
    }
    return {.status = Status::OK(), .dataSize = recordBson.objsize()};
}

size_t collection_validation::getNumberOfAdditionalCharactersForHashDrillDown(
    size_t numHashPrefixes, size_t hashPrefixLength) {
    // The maximum number of output buckets we can produce is determined by
    // the maximum BSON object size (16 MB) minus some buffer (50 KB) divided
    // by the size of a bucket entry.
    //
    // To calculate the size of a bucket entry in the output response, we
    // just construct an example entry BSON and then ask for its size.
    // {
    //     bucket: {'hash': <hash>, 'count': <int>}
    // }
    auto someHash = SHA256Block().toHexString();

    // Having numHashPrefixes == 0 or hashPrefixLength >= hash size is guaranteed
    // to hang / crash, so we terminate right away if they are violated.
    invariant(numHashPrefixes);
    invariant(hashPrefixLength <= someHash.size());

    // The length of a bucket is at least four characters long.
    auto bucketKeyLength = std::min(hashPrefixLength + 4, someHash.size());
    auto bucketKey = std::string(bucketKeyLength, 'a');
    auto singleBucketEntryDocument = BSON(bucketKey << BSON("hash" << someHash << "count" << 1));
    // We don't want to include the BSON metadata overhead that includes the size field and a
    // trailing '\0' in a full object, so we instead capture just the size of the bucket BSON
    // element.
    auto singleBucketSize = singleBucketEntryDocument.firstElement().size();

    // Reserving 50 KB of buffer room for everything else in the response.
    auto maxNumberOfBuckets =
        (static_cast<size_t>(BSONObjMaxUserSize - 50 * 1024)) / singleBucketSize;

    // With each additional hex character that we attach to a hashPrefix, we
    // end up spawning 16 child buckets.
    int numChars = 0;
    size_t currNumBuckets = numHashPrefixes;
    while (currNumBuckets < maxNumberOfBuckets) {
        numChars++;
        currNumBuckets = currNumBuckets * 16;
    }

    return numChars - 1;
}

void ValidateAdaptor::computeMetadataHash(OperationContext* opCtx,
                                          const CollectionPtr& coll,
                                          ValidateResults* results) {
    const auto& catalogEntry =
        MDBCatalog::get(opCtx)->getRawCatalogEntry(opCtx, coll->getCatalogId());
    // Zero out the initial hash.
    SHA256Block metadataHash;
    metadataHash.xorInline(metadataHash);
    for (const auto& field : catalogEntry) {
        auto fieldName = field.fieldNameStringData();
        if (fieldName == "ident") {
            metadataHash.xorInline(
                SHA256Block::computeHash({ConstDataRange(field.rawdata(), field.size())}));
        }
        if (fieldName == "idxIdent") {
            // XOR the hashes of subfields with 'metadataHash'.
            for (const auto& idxField : field.Obj()) {
                metadataHash.xorInline(SHA256Block::computeHash(
                    {ConstDataRange(idxField.rawdata(), idxField.size())}));
            }
        }
    }
    results->setMetadataHash(metadataHash.toHexString());
}

void ValidateAdaptor::hashDrillDown(OperationContext* opCtx, ValidateResults* results) {
    if (_validateState->getFirstRecordId().isNull()) {
        // The record store is empty if the first RecordId isn't initialized.
        return;
    }

    _numRecords = 0;
    ON_BLOCK_EXIT([&]() {
        results->setNumRecords(_numRecords);
        {
            std::unique_lock<Client> lk(*opCtx->getClient());
            _progress.get(lk)->finished();
        }
    });

    // Because the progress meter is intended as an approximation, it's sufficient to get the number
    // of records when we begin traversing, even if this number may deviate from the final number.
    const auto& coll = _validateState->getCollection();
    const char* curopMessage = "Validate: scanning documents for 'collHash' drill-down";
    const auto totalRecords = coll->getRecordStore()->numRecords();
    {
        std::unique_lock<Client> lk(*opCtx->getClient());
        _progress.set(lk, CurOp::get(opCtx)->setProgress(lk, curopMessage, totalRecords), opCtx);
    }

    const std::unique_ptr<SeekableRecordThrottleCursor>& traverseRecordStoreCursor =
        _validateState->getTraverseRecordStoreCursor();

    // Convert the vector of hashPrefixes provided to a set for easy lookup.
    const stdx::unordered_set<std::string> hashPrefixes(_validateState->getHashPrefixes()->begin(),
                                                        _validateState->getHashPrefixes()->end());
    auto prefixLength = _validateState->getHashPrefixes().get()[0].size();
    const size_t N = collection_validation::getNumberOfAdditionalCharactersForHashDrillDown(
        _validateState->getHashPrefixes()->size(), prefixLength);
    uassert(ErrorCodes::BadValue, "Too many hash prefixes provided.", N);
    // Searches through the list of hash prefixes for a prefix of the provided 'hash', which
    // is the hash of the _id field. If a matching prefix has been found, returns
    // <prefix> + N more characters. For example, given an _id hash "abcd", if a prefix
    // "ab" is found, and N=1, will return "abc".
    auto getPartialHashBucketKey = [&](const std::string& hash) -> boost::optional<std::string> {
        // All hash prefixes are assumed to be the same length.
        const auto idHashPrefix = hash.substr(0, prefixLength);
        if (hashPrefixes.contains(idHashPrefix)) {
            // Return the hash with up to N more characters. Calling this
            // with a value greater than the length of hash is safe.
            return hash.substr(0, prefixLength + N);
        }
        return boost::none;
    };

    // A map from an idHash prefix to the running hash of all documents in that bucket, plus the
    // number of documents.
    stdx::unordered_map<std::string, std::pair<SHA256Block, int>> idHashToDocHash;

    for (auto record =
             traverseRecordStoreCursor->seekExact(opCtx, _validateState->getFirstRecordId());
         record;
         record = traverseRecordStoreCursor->next(opCtx)) {
        {
            std::unique_lock<Client> lk(*opCtx->getClient());
            _progress.get(lk)->hit();
        }
        ++_numRecords;
        BSONObj recordBson = record->data.toBson();
        auto idField = recordBson["_id"];

        auto idBlock =
            SHA256Block::computeHash({ConstDataRange(idField.value(), idField.valuesize())});
        auto deeperHash = getPartialHashBucketKey(idBlock.toHexString());
        if (deeperHash) {
            auto docHash = SHA256Block::computeHash(
                {ConstDataRange(record->data.data(), record->data.size())});
            if (!idHashToDocHash.count(*deeperHash)) {
                idHashToDocHash.emplace(*deeperHash, std::make_pair(docHash, 1));
            } else {
                idHashToDocHash.at(*deeperHash).first.xorInline(docHash);
                idHashToDocHash.at(*deeperHash).second++;
            }
        }
    }

    // Dump the map into results and convert the SHA256 doc hashes to strings.
    stdx::unordered_map<std::string, std::pair<std::string, int>> partial;
    for (const auto& [prefix, hashAndCount] : idHashToDocHash) {
        partial.emplace(prefix,
                        std::make_pair(hashAndCount.first.toHexString(), hashAndCount.second));
    }

    results->setPartialHashes(std::move(partial));
}

void ValidateAdaptor::traverseRecordStore(OperationContext* opCtx,
                                          ValidateResults* results,
                                          ValidationVersion validationVersion) {
    _numRecords = 0;  // need to reset it because this function can be called more than once.
    long long dataSizeTotal = 0;
    long long interruptIntervalNumBytes = 0;
    long long nInvalid = 0;
    long long nNonCompliantDocuments = 0;
    long long numCorruptRecordsSizeBytes = 0;

    ON_BLOCK_EXIT([&]() {
        results->setNumInvalidDocuments(nInvalid);
        results->setNumNonCompliantDocuments(nNonCompliantDocuments);
        results->setNumRecords(_numRecords);
        {
            std::unique_lock<Client> lk(*opCtx->getClient());
            _progress.get(lk)->finished();
        }
    });

    RecordId prevRecordId;

    // In case validation occurs twice and the progress meter persists after index traversal
    {
        std::unique_lock<Client> lk(*opCtx->getClient());
        if (_progress.get(lk) && _progress.get(lk)->isActive()) {
            _progress.get(lk)->finished();
        }
    }

    // Because the progress meter is intended as an approximation, it's sufficient to get the number
    // of records when we begin traversing, even if this number may deviate from the final number.
    const auto& coll = _validateState->getCollection();
    const char* curopMessage = "Validate: scanning documents";
    const auto totalRecords = coll->getRecordStore()->numRecords();
    const auto rs = coll->getRecordStore();
    {
        std::unique_lock<Client> lk(*opCtx->getClient());
        _progress.set(lk, CurOp::get(opCtx)->setProgress(lk, curopMessage, totalRecords), opCtx);
    }

    // Place an empty hash in the results to override later. This result will only be used
    // for empty collections.
    if (_validateState->isCollHashValidation()) {
        results->setCollectionHash(SHA256Block::computeHash({}).toHexString());
    }

    if (_validateState->getFirstRecordId().isNull()) {
        // The record store is empty if the first RecordId isn't initialized.
        return;
    }

    const std::unique_ptr<SeekableRecordThrottleCursor>& traverseRecordStoreCursor =
        _validateState->getTraverseRecordStoreCursor();

    // Accumulates each record's SHA256 block as they are XORed together. Starts off
    // zeroed out.
    SHA256Block accumulatedBlock;
    accumulatedBlock.xorInline(accumulatedBlock);
    bool revealHashedIds = _validateState->getRevealHashedIds().has_value();
    stdx::unordered_map<std::string, std::vector<BSONObj>> revealedIds;
    if (revealHashedIds) {
        for (const auto& hashPrefix : _validateState->getRevealHashedIds().get()) {
            revealedIds[hashPrefix] = {};
        }
    }

    for (auto record =
             traverseRecordStoreCursor->seekExact(opCtx, _validateState->getFirstRecordId());
         record;
         record = traverseRecordStoreCursor->next(opCtx)) {
        {
            std::unique_lock<Client> lk(*opCtx->getClient());
            _progress.get(lk)->hit();
        }
        ++_numRecords;
        auto dataSize = record->data.size();
        interruptIntervalNumBytes += dataSize;
        dataSizeTotal += dataSize;
        const auto [validateRecordStatus, validatedSize, maybeValidateRecordErrorMessage] =
            validateRecord(opCtx,
                           record->id,
                           record->data,
                           nNonCompliantDocuments,
                           nInvalid,
                           results,
                           validationVersion);

        if (_validateState->isCollHashValidation()) {
            SHA256Block block = SHA256Block::computeHash(
                {ConstDataRange(record->data.data(), record->data.size())});
            accumulatedBlock.xorInline(block);
            if (revealHashedIds) {
                const auto idField = record->data.toBson()["_id"];
                auto idBlock = SHA256Block::computeHash(
                    {ConstDataRange(idField.value(), idField.valuesize())});
                for (const auto& hashPrefix : _validateState->getRevealHashedIds().get()) {
                    if (idBlock.toHexString().starts_with(hashPrefix)) {
                        revealedIds[hashPrefix].push_back(idField.wrap());
                    }
                }
            }
        }

        // Log the out-of-order entries as errors.
        //
        // Validate uses a DataCorruptionDetectionMode::kLogAndContinue mode such that data
        // corruption errors are logged without throwing, so certain checks must be duplicated here
        // as well.
        if ((prevRecordId.isValid() && prevRecordId > record->id) ||
            MONGO_unlikely(failRecordStoreTraversal.shouldFail())) {
            results->addError(kOutOfOrderDocumentError);
        }

        // validatedSize = dataSize is not a general requirement as some storage engines may use
        // padding, but we still require that they return the unpadded record data.
        if (!validateRecordStatus.isOK() || validatedSize != dataSize) {
            // If status is not okay, dataSize is not reliable.
            if (!validateRecordStatus.isOK()) {
                LOGV2_OPTIONS(4835001,
                              {logv2::LogTruncation::Disabled},
                              "Document corruption details - Document validation failed with error",
                              "recordId"_attr = record->id,
                              "error"_attr = validateRecordStatus);
            } else {
                LOGV2_OPTIONS(
                    4835002,
                    {logv2::LogTruncation::Disabled},
                    "Document corruption details - Document validation failure; size mismatch",
                    "recordId"_attr = record->id,
                    "validatedBytes"_attr = validatedSize,
                    "recordBytes"_attr = dataSize);
            }

            if (_validateState->fixErrors()) {
                WriteUnitOfWork wunit(opCtx);
                rs->deleteRecord(opCtx, *shard_role_details::getRecoveryUnit(opCtx), record->id);
                wunit.commit();
                results->setRepaired(true);
                results->addNumRemovedCorruptRecords(1);
                _numRecords--;
            } else {
                // If this is not set up to repair and remove the corrupt records, the error
                // returned from record Validation should be logged if it exists.
                if (!validateRecordStatus.isOK()) {
                    results->addError(
                        maybeValidateRecordErrorMessage.value_or(kInvalidDocumentError));
                }
                numCorruptRecordsSizeBytes += record->id.memUsage();
                if (numCorruptRecordsSizeBytes <= kMaxErrorSizeBytes) {
                    results->addCorruptRecord(record->id);
                } else {
                    results->addWarning(kNotEnoughSpaceToReportCorruptionWarning);
                }

                nInvalid++;
            }
        } else {
            // If the document is not corrupted, validate the document against this collection's
            // schema validator. Don't treat invalid documents as errors since documents can bypass
            // document validation when being inserted or updated.
            const auto [checkValidationResult, schemaValidationStatus] =
                coll->checkValidation(opCtx, record->data.toBson());

            // Timeseries collections are a special case. The schema is required and all violations
            // will be logged as errors instead.
            const bool isTimeseries = coll->getTimeseriesOptions().has_value();

            switch (checkValidationResult.result) {
                case Collection::SchemaValidationResult::kPass:
                    if (isTimeseries) {
                        // Timeseries documents checks cannot be run if schema validation fails.
                        const BSONObj recordBson = record->data.toBson();

                        // Checks for time-series collection consistency.
                        const auto timeseriesValidationResult =
                            collection_validation::validateTimeSeriesBucketRecord(
                                opCtx, *_validateState, coll, recordBson, *results);
                        // This log id should be kept in sync with the associated warning messages
                        // that are returned to the client.
                        switch (timeseriesValidationResult.result) {
                            case collection_validation::TimeseriesValidationResult::kValid:
                                break;
                            // We should not add data-annotated error strings to the set, since
                            // bucket-specific data can greatly increase the number of unique
                            // error strings stored; this set is not intended to scale with the
                            // number of documents. Bucket-specific data should instead be
                            // logged above.

                            // The following result cases are logged as warnings
                            case collection_validation::TimeseriesValidationResult::
                                kV3WithOrderedTime:
                                LOGV2_WARNING_OPTIONS(
                                    12351700,
                                    {logv2::LogTruncation::Disabled},
                                    "Document is not compliant with time-series specifications",
                                    logAttrs(coll->ns()),
                                    "recordId"_attr = record->id,
                                    "record"_attr = record->data.toBson(),
                                    "reason"_attr = timeseriesValidationResult.reason);
                                ++nNonCompliantDocuments;
                                results->addWarning(
                                    collection_validation::describeTimeseriesValidationResult(
                                        timeseriesValidationResult.result));
                                break;

                            // All remaining result cases are errors
                            default:
                                LOGV2_ERROR_OPTIONS(
                                    6698300,
                                    {logv2::LogTruncation::Disabled},
                                    "Document is not compliant with time-series specifications",
                                    logAttrs(coll->ns()),
                                    "recordId"_attr = record->id,
                                    "record"_attr = record->data.toBson(),
                                    "reason"_attr = timeseriesValidationResult.reason);
                                ++nNonCompliantDocuments;
                                results->addError(
                                    collection_validation::describeTimeseriesValidationResult(
                                        timeseriesValidationResult.result));
                        }
                        const auto containsMixedSchemaDataResponse =
                            coll->doesTimeseriesBucketsDocContainMixedSchemaData(recordBson);
                        if (!containsMixedSchemaDataResponse.isOK() &&
                            results->addError(
                                collection_validation::kMalformedMinMaxTimeseriesBucket)) {
                            LOGV2_WARNING_OPTIONS(
                                8469900,
                                {logv2::LogTruncation::Disabled},
                                collection_validation::kMalformedMinMaxTimeseriesBucket,
                                logAttrs(coll->ns()),
                                "recordId"_attr = record->id,
                                "record"_attr = record->data.toBson(),
                                "error"_attr = containsMixedSchemaDataResponse.getStatus());
                        } else if (containsMixedSchemaDataResponse.isOK() &&
                                   containsMixedSchemaDataResponse.getValue()) {
                            const bool mixedSchemaAllowed =
                                coll->getTimeseriesMixedSchemaBucketsState()
                                    .canStoreMixedSchemaBucketsSafely();
                            if (mixedSchemaAllowed &&
                                results->addWarning(
                                    collection_validation::kExpectedMixedSchemaTimeseriesWarning)) {
                                LOGV2_WARNING_OPTIONS(
                                    8469901,
                                    {logv2::LogTruncation::Disabled},
                                    collection_validation::kExpectedMixedSchemaTimeseriesWarning,
                                    logAttrs(coll->ns()),
                                    "recordId"_attr = record->id);
                            } else if (!mixedSchemaAllowed &&
                                       results->addError(
                                           collection_validation::
                                               kUnexpectedMixedSchemaTimeseriesError)) {
                                const auto& controlField =
                                    recordBson.getField(timeseries::kBucketControlFieldName).Obj();
                                const int count = controlField.getIntField(
                                    timeseries::kBucketControlCountFieldName);
                                LOGV2_WARNING_OPTIONS(
                                    8469902,
                                    {logv2::LogTruncation::Disabled},
                                    collection_validation::kUnexpectedMixedSchemaTimeseriesError,
                                    logAttrs(coll->ns()),
                                    "recordId"_attr = record->id,
                                    "record"_attr = record->data.toBson(),
                                    "objSize"_attr = recordBson.objsize(),
                                    "measurementCount"_attr = count);
                            }
                        }
                    }
                    break;
                case Collection::SchemaValidationResult::kWarn:
                case Collection::SchemaValidationResult::kError:
                case Collection::SchemaValidationResult::kErrorAndLog: {
                    // Non-kPass results indicate a schema validation failure. Do not add
                    // data-annotated strings to the set, since per-document data can greatly
                    // increase the number of unique strings stored; this set is not intended
                    // to scale with the number of documents. Document-specific data is logged.
                    ++nNonCompliantDocuments;
                    const char* description =
                        _describeDocumentValidationResult(checkValidationResult);
                    if (isTimeseries) {
                        LOGV2_WARNING_OPTIONS(11634800,
                                              {logv2::LogTruncation::Disabled},
                                              "Time-series bucket document is not compliant with "
                                              "time-series specifications",
                                              logAttrs(coll->ns()),
                                              "recordId"_attr = record->id,
                                              "collectionUUID"_attr = coll->uuid(),
                                              "record"_attr = record->data.toBson(),
                                              "reason"_attr = description);
                        results->addError(description);
                    } else {
                        LOGV2_WARNING_OPTIONS(
                            5363500,
                            {logv2::LogTruncation::Disabled},
                            "Document is not compliant with the collection's schema",
                            logAttrs(coll->ns()),
                            "recordId"_attr = record->id,
                            "reason"_attr = description);
                        results->addWarning(description);
                    }
                    break;
                }
            }
        }

        prevRecordId = record->id;

        if (_numRecords % IndexConsistency::kInterruptIntervalNumRecords == 0 ||
            interruptIntervalNumBytes >= kInterruptIntervalNumBytes) {
            // Periodically checks for interrupts and yields.
            opCtx->checkForInterrupt();
            _validateState->yieldCursors(opCtx);

            if (interruptIntervalNumBytes >= kInterruptIntervalNumBytes) {
                interruptIntervalNumBytes = 0;
            }
        }
    }

    if (_validateState->isCollHashValidation()) {
        results->setCollectionHash(accumulatedBlock.toHexString());
        if (revealHashedIds) {
            results->setRevealedIds(std::move(revealedIds));
        }
    }

    if (results->getNumRemovedCorruptRecords() > 0) {
        results->addWarning(str::stream() << "Removed " << results->getNumRemovedCorruptRecords()
                                          << " invalid documents.");
    }

    const collection_validation::FastCountType fastCountType =
        _validateState->getDetectedFastCountType(opCtx);
    if (_validateState->shouldEnforceFastCount(opCtx, fastCountType)) {
        if (const auto fastCount = coll->latestSizeCount(opCtx).count; fastCount != _numRecords) {
            results->addError(
                fmt::format("fast count ({}) does not match number of "
                            "records ({}) for collection '{}' with fast count store type '{}'",
                            fastCount,
                            _numRecords,
                            coll->ns().toStringForErrorMsg(),
                            toString(fastCountType)));
        }
    }

    if (_validateState->shouldEnforceFastSize(opCtx, fastCountType)) {
        if (const auto fastSize = coll->latestSizeCount(opCtx).size; fastSize != dataSizeTotal) {
            results->addError(
                fmt::format("fast size ({}) does not match data size ({}) "
                            "for collection '{}' with fast count store type '{}'",
                            fastSize,
                            dataSizeTotal,
                            coll->ns().toStringForErrorMsg(),
                            toString(fastCountType)));
        }
    }

    // TODO(SERVER-119193): Add condition for the fastCountType being valid.
    // Do not update the record store stats if we're in the background as we've validated a
    // checkpoint and it may not have the most up-to-date changes.
    if (results->isValid() && !_validateState->isBackground()) {
        coll->getRecordStore()->updateStatsAfterRepair(_numRecords, dataSizeTotal);
    }
}

void ValidateAdaptor::validateIndexKeyCount(OperationContext* opCtx,
                                            const IndexCatalogEntry* index,
                                            IndexValidateResults& results) {
    _keyBasedIndexConsistency.validateIndexKeyCount(opCtx, index, &_numRecords, results);
}

void ValidateAdaptor::traverseIndex(OperationContext* opCtx,
                                    const IndexCatalogEntry* index,
                                    int64_t* numTraversedKeys,
                                    ValidateResults* results) {
    // The progress meter will be inactive after traversing the record store to allow the
    // message and the total to be set to different values.
    {
        std::unique_lock<Client> lk(*opCtx->getClient());
        if (!_progress.get(lk)->isActive()) {
            const char* curopMessage = "Validate: scanning index entries";
            _progress.set(lk,
                          CurOp::get(opCtx)->setProgress(
                              lk, curopMessage, _keyBasedIndexConsistency.getTotalIndexKeys()),
                          opCtx);
        }
    }

    int64_t numKeys = _keyBasedIndexConsistency.traverseIndex(opCtx, index, _progress, results);

    if (numTraversedKeys) {
        *numTraversedKeys = numKeys;
    }
}

void ValidateAdaptor::traverseRecord(OperationContext* opCtx,
                                     const CollectionPtr& coll,
                                     const IndexCatalogEntry* index,
                                     const RecordId& recordId,
                                     const BSONObj& record,
                                     ValidateResults* results) {
    _keyBasedIndexConsistency.traverseRecord(opCtx, coll, index, recordId, record, results);
}

void ValidateAdaptor::setSecondPhase() {
    _keyBasedIndexConsistency.setSecondPhase();
}

bool ValidateAdaptor::limitMemoryUsageForSecondPhase(ValidateResults* result) {
    return _keyBasedIndexConsistency.limitMemoryUsageForSecondPhase(result);
}

bool ValidateAdaptor::haveEntryMismatch() const {
    return _keyBasedIndexConsistency.haveEntryMismatch();
}

void ValidateAdaptor::repairIndexEntries(OperationContext* opCtx, ValidateResults* results) {
    _keyBasedIndexConsistency.repairIndexEntries(opCtx, results);
}

void ValidateAdaptor::addIndexEntryErrors(OperationContext* opCtx, ValidateResults* results) {
    _keyBasedIndexConsistency.addIndexEntryErrors(opCtx, results);
}

}  // namespace mongo
