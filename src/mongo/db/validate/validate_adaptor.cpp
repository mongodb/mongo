// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/validate/validate_adaptor.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
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
#include "mongo/db/repl/internode_validation_hash_utils.h"
#include "mongo/db/rss/persistence_provider.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/server_feature_flags_gen.h"
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
#include "mongo/db/validate/key_string_index_consistency.h"
#include "mongo/db/validate/record_store_slicer.h"
#include "mongo/db/validate/validate_gen.h"
#include "mongo/db/validate/validate_timeseries.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/object_check.h"  // IWYU pragma: keep
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/shared_buffer_fragment.h"
#include "mongo/util/str.h"
#include "mongo/util/testing_proctor.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>

#include <absl/container/flat_hash_map.h>
#include <boost/container/small_vector.hpp>
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
        case NCR::kTimeseriesSchemaViolation: {
            using BCV = Collection::DocumentValidationResult::BucketConsistencyViolation;
            switch (cvr.bucketViolation) {
                case BCV::kNone:
                    return cvr.result == SVR::kErrorAndLog
                        ? "Detected one or more time-series bucket documents not compliant with "
                          "time-series specifications with 'errorAndLog' validation action. Check "
                          "logs for log id 11634800."
                        : "Detected one or more time-series bucket documents not compliant with "
                          "time-series specifications with 'error' validation action. Check logs "
                          "for log id 11634800.";
                case BCV::kBadVersion:
                    return "Detected one or more time-series bucket documents with an invalid "
                           "'control.version' field (expected 1, 2, or 3). Check logs for log id "
                           "11634800.";
                case BCV::kIdTimestampMismatch:
                    return "Detected one or more time-series bucket documents where the timestamp "
                           "embedded in '_id' does not match 'control.min' timestamp. Check logs "
                           "for log id 11634800.";
                case BCV::kTimeSpanTooLarge:
                    return "Detected one or more time-series bucket documents whose time span "
                           "between 'control.min' and 'control.max' exceeds the collection's "
                           "bucketMaxSpanSeconds. Check logs for log id 11634800.";
                case BCV::kMinTimeNotRounded:
                    return "Detected one or more time-series bucket documents where "
                           "'control.min' time is not aligned to the fixed-bucket boundary. "
                           "Check logs for log id 11634800.";
                case BCV::kDuplicateField:
                    return "Detected one or more time-series bucket documents containing a "
                           "duplicate field in the data object. Check logs for log id 11634800.";
                case BCV::kFieldCountMismatch:
                    return "Detected one or more time-series bucket documents where the number of "
                           "data fields does not match the number of 'control.min' or "
                           "'control.max' fields. Check logs for log id 11634800.";
                case BCV::kMissingField:
                    return "Detected one or more time-series bucket documents with a data field "
                           "that has no corresponding entry in 'control.min' or 'control.max'. "
                           "Check logs for log id 11634800.";
                case BCV::kMissingTimeField:
                    return "Detected one or more time-series bucket documents where the "
                           "collection's time field is absent from the 'data' object. Check logs "
                           "for log id 11634800.";
                case BCV::kBadControlCount:
                    return "Detected one or more time-series bucket documents with an invalid "
                           "'control.count' value. Check logs for log id 11634800.";
                case BCV::kIndexNotIncreasing:
                    return "Detected one or more time-series bucket documents with data field "
                           "indexes that are not consecutively increasing from 0. Check logs for "
                           "log id 11634800.";
                case BCV::kIndexOutOfRange:
                    return "Detected one or more time-series bucket documents with a data field "
                           "index that exceeds the measurement count. Check logs for log id "
                           "11634800.";
                case BCV::kIndexBadValue:
                    return "Detected one or more time-series bucket documents with a negative or "
                           "non-numerical data field index. Check logs for log id 11634800.";
                case BCV::kMinMaxMismatch:
                    return "Detected one or more time-series bucket documents where the observed "
                           "data min or max does not match 'control.min' or 'control.max'. Check "
                           "logs for log id 11634800.";
                case BCV::kBadDataType:
                    return "Detected one or more time-series bucket documents with a compressed "
                           "data field of unexpected BSON type (expected binData). Check logs for "
                           "log id 11634800.";
                case BCV::kBadBinDataSubtype:
                    return "Detected one or more time-series bucket documents with a compressed "
                           "data field of unexpected binData subtype (expected Column). Check logs "
                           "for log id 11634800.";
                case BCV::kCountMismatch:
                    return "Detected one or more time-series bucket documents where the "
                           "decompressed column element count does not match 'control.count'. "
                           "Check logs for log id 11634800.";
                case BCV::kInvalidBsonData:
                    return "Detected one or more time-series bucket documents with missing "
                           "required fields, unexpected field types, or malformed compressed "
                           "column data. Check logs for log id 11634800.";
            }
            MONGO_UNREACHABLE;
        }
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

/**
 * Helper for validating collection's fast count and fast size against the totals accumulated by a
 * complete record store traversal.
 */
void _validateFastCountAndSize(OperationContext* opCtx,
                               const collection_validation::ValidateState& validateState,
                               const Collection& coll,
                               int64_t numRecords,
                               int64_t dataSizeTotal,
                               ValidateResults& results) {
    const collection_validation::FastCountType fastCountType =
        validateState.getDetectedFastCountType(opCtx);
    if (validateState.shouldEnforceFastCount(opCtx, fastCountType)) {
        if (const auto fastCount = coll.latestSizeCount(opCtx).count; fastCount != numRecords) {
            results.addError(
                fmt::format("fast count ({}) does not match number of "
                            "records ({}) for collection '{}' with fast count store type '{}'",
                            fastCount,
                            numRecords,
                            coll.ns().toStringForErrorMsg(),
                            toString(fastCountType)));
        }
    }

    if (validateState.shouldEnforceFastSize(opCtx, fastCountType)) {
        if (const auto fastSize = coll.latestSizeCount(opCtx).size; fastSize != dataSizeTotal) {
            results.addError(
                fmt::format("fast size ({}) does not match data size ({}) "
                            "for collection '{}' with fast count store type '{}'",
                            fastSize,
                            dataSizeTotal,
                            coll.ns().toStringForErrorMsg(),
                            toString(fastCountType)));
        }
    }
}

/**
 * Reads through the ValidateState's shared cursor, which throttles its reads and yields its
 * snapshot periodically. This is the policy serial validation has always run under, and it is
 * single-threaded by construction: neither the cursor nor the DataThrottle behind it is
 * thread-safe.
 */
class ThrottledValidateCursor final : public ValidateCursor {
public:
    ThrottledValidateCursor(OperationContext* opCtx,
                            std::shared_ptr<SeekableRecordThrottleCursor> cursor)
        : _opCtx(opCtx), _cursor(std::move(cursor)) {}

    boost::optional<Record> seek(const RecordId& recordId) override {
        // The whole-record-store traversal begins on a RecordId the ValidateState read off the
        // record store, so the record is required to be there.
        return _cursor->seekExact(_opCtx, recordId);
    }

    boost::optional<Record> next() override {
        return _cursor->next(_opCtx);
    }

    void yield() override {
        _cursor->save();
        uassert(ErrorCodes::Interrupted,
                "Interrupted due to: failure to restore yielded traverse cursor",
                _cursor->restore(*shard_role_details::getRecoveryUnit(_opCtx)));
    }

private:
    OperationContext* _opCtx;
    std::shared_ptr<SeekableRecordThrottleCursor> _cursor;
};

/**
 * Reads through a cursor of its own, unthrottled and without yielding, so that it can run on a
 * worker thread alongside the other slices of a parallel scan. The parallel path is restricted to
 * foreground validation, where throttling is off regardless, so nothing is lost by bypassing it.
 */
class SliceValidateCursor final : public ValidateCursor {
public:
    SliceValidateCursor(std::unique_ptr<SeekableRecordCursor> cursor)
        : _cursor(std::move(cursor)) {}

    boost::optional<Record> seek(const RecordId& recordId) override {
        return _cursor->seek(recordId, SeekableRecordCursor::BoundInclusion::kInclude);
    }

    boost::optional<Record> next() override {
        return _cursor->next();
    }

    void yield() override {}

private:
    std::unique_ptr<SeekableRecordCursor> _cursor;
};

}  // namespace

auto ValidateAdaptor::validateRecord(OperationContext* opCtx,
                                     const Record& record,
                                     ValidateResults& results,
                                     KeyStringIndexConsistency& keyStringIndexConsistency,
                                     std::span<const IndexCatalogEntry*> indexCatalogEntries,
                                     ValidationVersion validationVersion) const
    -> ValidateRecordResult {
    bool compliantDocument{true};
    bool validDocument{true};
    {
        const Status bsonValidationStatus = validateBSON(record.data.data(),
                                                         record.data.size(),
                                                         _validateState->getBSONValidateMode(),
                                                         validationVersion);

        if (!bsonValidationStatus.isOK()) {
            bool includeReason{false};
            switch (bsonValidationStatus.code()) {
                case ErrorCodes::NonConformantBSON:
                    LOGV2_WARNING_OPTIONS(6825900,
                                          {logv2::LogTruncation::Disabled},
                                          "Document is not conformant to BSON specifications",
                                          "recordId"_attr = record.id,
                                          "reason"_attr = bsonValidationStatus);
                    compliantDocument = false;
                    results.addWarning(kBSONValidationNonConformantReason);
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
                                        "recordId"_attr = record.id,
                                        "reason"_attr = bsonValidationStatus);
                    return {.status = bsonValidationStatus,
                            .errorMessage = fmt::format(
                                "BSON validation failed with error '{}'{}. For more info, "
                                "see logs "
                                "with log id 12395400",
                                ErrorCodes::errorString(bsonValidationStatus.code()),
                                includeReason ? fmt::format(": {}", bsonValidationStatus.reason())
                                              : std::string("")),
                            .compliantDocument = compliantDocument,
                            .validDocument = validDocument};
            }
        } else if (!_validateState->nss().isOplog()) {
            // Additionally check size if the BSON object is compliant. Do not run this check on the
            // oplog as entries are expected to exceed the max allowed user size. Use the internal
            // size for internal collections.
            const auto objSizeLimit = _validateState->nss().isOnInternalDb()
                ? BSONObjMaxInternalSize
                : BSONObjMaxUserSize;
            Status sizeValidationStatus = record.data.toBson().validateBSONObjSize(objSizeLimit);

            if (!sizeValidationStatus.isOK()) {
                if (sizeValidationStatus.code() == ErrorCodes::BSONObjectTooLarge) {
                    LOGV2_ERROR_OPTIONS(10869900,
                                        {logv2::LogTruncation::Disabled},
                                        "Document BSON object is too large.",
                                        "recordId"_attr = record.id,
                                        "ns"_attr = _validateState->nss(),
                                        "reason"_attr = sizeValidationStatus);
                    validDocument = false;
                    results.addError(kBSONValidationObjectTooLargeReason,
                                     /*stopValidation=*/false);
                } else {
                    // Error is not related to BSON size limitations.
                    return {.status = sizeValidationStatus,
                            .errorMessage = boost::none,
                            .compliantDocument = compliantDocument,
                            .validDocument = validDocument};
                }
            }
        }
    }

    const BSONObj recordBson = record.data.toBson();

    if (MONGO_unlikely(_validateState->logDiagnostics())) {
        LOGV2(4666601, "[validate]", "recordId"_attr = record.id, "recordData"_attr = recordBson);
    }

    const CollectionPtr& coll = _validateState->getCollection();
    if (coll->isClustered()) {
        _validateClusteredCollectionRecordId(opCtx,
                                             record.id,
                                             recordBson,
                                             coll->getClusteredInfo()->getIndexSpec(),
                                             coll->getDefaultCollator(),
                                             &results);
    }

    for (const auto* indexEntry : indexCatalogEntries) {
        if ((indexEntry->descriptor()->isPartial() &&
             !exec::matcher::matchesBSON(indexEntry->getFilterExpression(), recordBson)) ||
            !results.getIndexValidateResult(indexEntry->descriptor()->indexName())
                 .continueValidation()) {
            continue;
        }

        keyStringIndexConsistency.traverseRecord(
            opCtx, coll, indexEntry, record.id, recordBson, &results);
    }
    return {.status = Status::OK(),
            .dataSize = recordBson.objsize(),
            .compliantDocument = compliantDocument,
            .validDocument = validDocument};
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
    results->setMetadataHash(metadataHash);
}

void ValidateAdaptor::hashDrillDown(OperationContext* opCtx, ValidateResults* results) {
    if (_validateState->getFirstRecordId().isNull()) {
        // The record store is empty if the first RecordId isn't initialized.
        return;
    }

    _numRecords = 0;
    ON_BLOCK_EXIT([&]() {
        results->setNumRecords(_numRecords);
        _progress.finished();
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

    auto traverseRecordStoreCursor = _validateState->getTraverseRecordStoreCursor();

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
        _progress.hit();
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
                                          ValidateResults& validateResults,
                                          ValidationVersion validationVersion,
                                          boost::optional<int64_t> targetRecordsPerSlice) {
    const auto& coll = _validateState->getCollection();
    const int64_t totalRecords = coll->getRecordStore()->numRecords();

    // In case validation occurs twice and the progress meter persists after index traversal.
    const bool progressStillActive = ([&] {
        std::unique_lock<Client> lk(*opCtx->getClient());
        return _progress.get(lk) && _progress.get(lk)->isActive();
    })();
    if (progressStillActive) {
        _progress.finished();
    }

    const char* curopMessage = "Validate: scanning documents";
    {
        std::unique_lock<Client> lk(*opCtx->getClient());
        _progress.set(lk, CurOp::get(opCtx)->setProgress(lk, curopMessage, totalRecords), opCtx);
    }
    ON_BLOCK_EXIT([&]() { _progress.finished(); });

    _numRecords = 0;
    int64_t dataSizeTotal{0};
    int64_t numRecords{0};

    // Split the record store into slices targeting ~targetRecordsPerSlice records each. Small
    // collections collapse to a single slice and run single-threaded, larger collections are split
    // into enough slices to keep the worker pool busy and allow "work stealing", as no guarantee
    // can be made that the cost of each slice is equal.

    const int64_t sliceCount = targetRecordsPerSlice.has_value() && (*targetRecordsPerSlice > 0)
        ? std::clamp<int64_t>((totalRecords + *targetRecordsPerSlice - 1) / *targetRecordsPerSlice,
                              1,
                              gValidateParallelMaxRecordStoreSlices.load())
        : 1;

    const bool hashDrillDown = _validateState->getHashPrefixes().has_value() ||
        _validateState->getRevealHashedIds().has_value();
    const bool parallelEligible = !_validateState->isBackground() &&
        _validateState->getRepairMode() == collection_validation::RepairMode::kNone &&
        _keyBasedIndexConsistency.canMergeResults() && !hashDrillDown && sliceCount > 1;

    const std::vector<RecordId> rsSlicePivots = parallelEligible
        ? collection_validation::computeSlicePivots(opCtx,
                                                    *shard_role_details::getRecoveryUnit(opCtx),
                                                    *coll->getRecordStore(),
                                                    sliceCount)
        : std::vector<RecordId>{};
    const bool synchronousTraversal = rsSlicePivots.size() <= 1;

    if (synchronousTraversal) {
        auto implResults = traverseRecordStoreImpl(
            opCtx,
            validateResults,
            _keyBasedIndexConsistency,
            {.beginRecordId = _validateState->getFirstRecordId(),
             .cursorFactory =
                 [this](OperationContext* traversalOpCtx) -> std::unique_ptr<ValidateCursor> {
                 return std::make_unique<ThrottledValidateCursor>(
                     traversalOpCtx, _validateState->getTraverseRecordStoreCursor());
             }},
            _progress,
            validationVersion);
        validateResults = std::move(implResults.validateResults);
        _keyBasedIndexConsistency = std::move(implResults.keyStringIndexConsistency);
        dataSizeTotal = implResults.dataSizeTotal;
        numRecords = validateResults.getNumRecords().value_or(0);
        uassertStatusOK(implResults.status);
    } else {
        // Turn the pivots into half-open [begin, end) ranges, with the final slice left open-ended
        // so that the ranges cover the whole record store no matter how many pivots the slicer
        // produced. Deduplication of the pivots means there may be fewer slices than 'sliceCount'.
        //
        // Every slice gets the same factory, which each worker calls on its own thread to open a
        // cursor of its own.
        const ValidateCursor::Factory sliceCursorFactory =
            [recordStore = _validateState->getCollection()->getRecordStore()](
                OperationContext* traversalOpCtx) -> std::unique_ptr<ValidateCursor> {
            return std::make_unique<SliceValidateCursor>(recordStore->getCursor(
                traversalOpCtx, *shard_role_details::getRecoveryUnit(traversalOpCtx)));
        };
        const auto trsOpts = ([&rsSlicePivots, &sliceCursorFactory] {
            std::vector<TraverseRecordStoreOptions> trsOpts;
            auto itRangeBegin = rsSlicePivots.begin();
            auto itRangeEnd = rsSlicePivots.begin();
            ++itRangeEnd;
            while (itRangeEnd != rsSlicePivots.end()) {
                trsOpts.push_back({.beginRecordId = *itRangeBegin,
                                   .endRecordId = *itRangeEnd,
                                   .cursorFactory = sliceCursorFactory});
                ++itRangeBegin;
                ++itRangeEnd;
            }
            trsOpts.push_back(
                {.beginRecordId = *itRangeBegin, .cursorFactory = sliceCursorFactory});
            return trsOpts;
        })();

        {
            std::vector<PromiseAndFuture<TraverseRecordStoreResults>> rsSliceResults(
                trsOpts.size());

            const size_t maxThreads =
                std::min(trsOpts.size(), static_cast<size_t>(ProcessInfo::getNumAvailableCores()));

            ThreadPool threadPool({.poolName = "ValidateRecordStore",
                                   .threadNamePrefix = "vrs",
                                   .maxThreads = maxThreads,
                                   .onCreateThread = [](const std::string& threadName) {
                                       Client::initThread(threadName,
                                                          getGlobalServiceContext()->getService());
                                   }});
            threadPool.startup();
            for (size_t i = 0; i < trsOpts.size(); ++i) {
                threadPool.schedule(
                    [this, &rsSliceResults, i, validationVersion, opts = trsOpts[i]](
                        Status status) {
                        if (!status.isOK()) {
                            rsSliceResults[i].promise.setError(status);
                            return;
                        }
                        try {
                            auto workerOpCtxHolder = cc().makeOperationContext();
                            auto* workerOpCtx = workerOpCtxHolder.get();
                            // Slice starts from an empty ValidateResults rather than the
                            // caller's; traverseRecordStoreImpl() stamps the identity fields that
                            // merge() requires to match.
                            auto innerResults = traverseRecordStoreImpl(workerOpCtx,
                                                                        ValidateResults{},
                                                                        _keyBasedIndexConsistency,
                                                                        opts,
                                                                        _progress,
                                                                        validationVersion);
                            rsSliceResults[i].promise.emplaceValue(std::move(innerResults));

                        } catch (const DBException& e) {
                            rsSliceResults[i].promise.setError(e.toStatus());
                        }
                    });
            }
            threadPool.waitForIdle();
            threadPool.shutdown();
            threadPool.join();

            std::vector<TraverseRecordStoreResults> implResults;
            implResults.reserve(rsSliceResults.size());
            std::vector<Status> sliceFailures;
            for (auto& pf : rsSliceResults) {
                auto swResults = std::move(pf.future).getNoThrow(opCtx);
                if (!swResults.isOK()) {
                    sliceFailures.push_back(swResults.getStatus());
                    continue;
                }
                implResults.push_back(std::move(swResults.getValue()));
            }

            // Combine the per-slice results before surfacing any slice's failure, so that a partial
            // failure still reports everything the other slices accumulated.
            for (const auto& ir : implResults) {
                validateResults.merge(ir.validateResults);
                _keyBasedIndexConsistency.merge(ir.keyStringIndexConsistency);
                dataSizeTotal += ir.dataSizeTotal;
                numRecords += ir.validateResults.getNumRecords().value_or(0);
            }
            for (const auto& ir : implResults) {
                if (!ir.status.isOK()) {
                    sliceFailures.push_back(ir.status);
                }
            }

            // Surface every slice's failures.
            if (!sliceFailures.empty()) {
                for (const auto& status : sliceFailures) {
                    LOGV2_ERROR(11157101,
                                "Record store slice traversal failed",
                                logAttrs(coll->ns()),
                                "error"_attr = status);
                }
                const auto& reported = *std::min_element(
                    sliceFailures.begin(), sliceFailures.end(), [](const auto& a, const auto& b) {
                        // An interrupt outranks the failures it caused in the other slices.
                        return ErrorCodes::isCancellationError(a.code()) &&
                            !ErrorCodes::isCancellationError(b.code());
                    });
                std::vector<std::string> reasons;
                reasons.reserve(sliceFailures.size());
                std::transform(sliceFailures.begin(),
                               sliceFailures.end(),
                               std::back_inserter(reasons),
                               [](const Status& status) { return status.toString(); });
                uasserted(reported.code(),
                          fmt::format("{} of {} record store slices failed to traverse: {}",
                                      sliceFailures.size(),
                                      trsOpts.size(),
                                      fmt::join(reasons, "; ")));
            }

            // Sanity check on slice coverage; fast count is an approximation, so this is a warning
            // rather than an error.
            if (const auto fastCount = coll->numRecords(opCtx); fastCount != numRecords) {
                LOGV2_WARNING(11157100,
                              "Parallel validation traversed a different number of records than "
                              "the collection's fast count",
                              logAttrs(coll->ns()),
                              "recordsTraversed"_attr = numRecords,
                              "fastCount"_attr = fastCount,
                              "numSlices"_attr = trsOpts.size());
            }
        }
    }

    _numRecords = numRecords;

    if (_validateState->getFirstRecordId().isNull()) {
        // The collection-level checks below all require the totals from a real traversal, so skip
        // them entirely rather than run them against zeroed counters. The parallel branch never
        // reaches here with a null first RecordId.
        return;
    }

    if (validateResults.getNumRemovedCorruptRecords() > 0) {
        validateResults.addWarning(fmt::format("Removed {} invalid documents.",
                                               validateResults.getNumRemovedCorruptRecords()));
    }

    _validateFastCountAndSize(
        opCtx, *_validateState, *coll.get(), _numRecords, dataSizeTotal, validateResults);

    // TODO(SERVER-119193): Add condition for the fastCountType being valid.
    // Do not update the record store stats if we're in the background as we've validated a
    // checkpoint and it may not have the most up-to-date changes.
    if (validateResults.isValid() && !_validateState->isBackground()) {
        coll->getRecordStore()->updateStatsAfterRepair(_numRecords, dataSizeTotal);
    }
}

auto ValidateAdaptor::traverseRecordStoreImpl(OperationContext* opCtx,
                                              const ValidateResults& baseResults,
                                              const KeyStringIndexConsistency& baseConsistency,
                                              TraverseRecordStoreOptions opts,
                                              ConcurrentProgressMeterHolder& progress,
                                              ValidationVersion validationVersion) const
    -> TraverseRecordStoreResults {
    // The traversal accumulates onto copies of the caller's state; the caller decides what to do
    // with them once the traversal returns.
    TraverseRecordStoreResults results{.validateResults = baseResults,
                                       .keyStringIndexConsistency = baseConsistency};

    // These identity fields (namespace, UUID, repair mode, read timestamp) are used to match
    // the results object this gets merged into.
    results.validateResults.setNamespaceString(_validateState->nss());
    results.validateResults.setUUID(_validateState->uuid());
    results.validateResults.setRepairMode(_validateState->getRepairMode());
    results.validateResults.setReadTimestamp(_validateState->getReadTimestamp());

    RecordId prevRecordId;

    results.validateResults.setNumRecords(0);
    results.validateResults.setNumInvalidDocuments(0);
    results.validateResults.setNumNonCompliantDocuments(0);

    const bool computeXxh3Hash = _validateState->isCollHashValidation() &&
        rss::ReplicatedStorageService::get(opCtx)
            .getPersistenceProvider()
            .shouldUseContinuousInternodeValidation();

    if (opts.beginRecordId.isNull()) {
        // The record store is empty if the first RecordId isn't initialized. Stand in an empty
        // hash, which is the correct collection hash for an empty collection.
        if (_validateState->isCollHashValidation()) {
            results.validateResults.setCollectionHash(SHA256Block::computeHash({}));
        }
        if (computeXxh3Hash) {
            results.validateResults.setXxh3CollectionHash(0);
        }
        return results;
    }

    try {
        // The caller decides what this traversal reads through, and with it whether reads are
        // throttled and whether the snapshot is yielded, as record store cursors belong to the
        // thread that uses them.
        const std::unique_ptr<ValidateCursor> cursor = opts.cursorFactory(opCtx);

        // Accumulates each record's SHA256 block as they are XORed together. Starts off
        // zeroed out.
        SHA256Block accumulatedBlock;
        accumulatedBlock.xorInline(accumulatedBlock);

        // Accumulates the same records' XXH3 hashes, XORed together the same way the replicated
        // collection validation hash folds in its per-document hashes. Zero is both the XOR
        // identity and the value that hash carries for an empty collection.
        uint64_t accumulatedXxh3 = 0;

        // Set when a record that failed validation was folded into the hashes, which then cover
        // that record's raw bytes rather than a document.
        bool hashedCorruptRecord = false;
        bool revealHashedIds = _validateState->getRevealHashedIds().has_value();
        stdx::unordered_map<std::string, std::vector<BSONObj>> revealedIds;
        if (revealHashedIds) {
            for (const auto& hashPrefix : _validateState->getRevealHashedIds().get()) {
                revealedIds[hashPrefix] = {};
            }
        }

        // Acquire index catalog entries once to avoid repeated findIndexByIdent() per document.
        static constexpr size_t kStackAllocatedSize{8};
        boost::container::small_vector<const IndexCatalogEntry*, kStackAllocatedSize> indexEntries;
        const auto& indexIdents = _validateState->getIndexIdents();
        indexEntries.reserve(indexIdents.size());
        const auto& coll = _validateState->getCollection();
        const auto rs = coll->getRecordStore();

        for (const auto& indexIdent : indexIdents) {
            indexEntries.push_back(coll->getIndexCatalog()->findIndexByIdent(opCtx, indexIdent));
        }

        int64_t interruptIntervalNumBytes = 0;
        int64_t numCorruptRecordsSizeBytes = 0;
        for (auto record = cursor->seek(opts.beginRecordId);
             record && (opts.endRecordId.isNull() || record->id < opts.endRecordId);
             record = cursor->next()) {

            const auto dataSize = record->data.size();
            interruptIntervalNumBytes += dataSize;
            results.dataSizeTotal += dataSize;
            results.validateResults.incrementNumRecords(1);
            progress.hit();
            const auto [validateRecordStatus,
                        validatedSize,
                        maybeValidateRecordErrorMessage,
                        compliantDocument,
                        validDocument] = validateRecord(opCtx,
                                                        record.value(),
                                                        results.validateResults,
                                                        results.keyStringIndexConsistency,
                                                        indexEntries,
                                                        validationVersion);
            results.validateResults.incrementNumNonCompliantDocuments(compliantDocument ? 0 : 1);
            results.validateResults.incrementNumInvalidDocuments(validDocument ? 0 : 1);

            if (_validateState->isCollHashValidation()) {
                const ConstDataRange recordRange(record->data.data(), record->data.size());
                SHA256Block block = SHA256Block::computeHash({recordRange});
                accumulatedBlock.xorInline(block);
                if (computeXxh3Hash) {
                    accumulatedXxh3 ^=
                        static_cast<uint64_t>(repl::computeDocValidationHash(recordRange));
                }

                hashedCorruptRecord |=
                    !validateRecordStatus.isOK() || validatedSize != dataSize || !validDocument;
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
            // corruption errors are logged without throwing, so certain checks must be
            // duplicated here as well.
            if ((prevRecordId.isValid() && prevRecordId > record->id) ||
                MONGO_unlikely(failRecordStoreTraversal.shouldFail())) {
                results.validateResults.addError(kOutOfOrderDocumentError);
            }

            // validatedSize = dataSize is not a general requirement as some storage engines may use
            // padding, but we still require that they return the unpadded record data.
            if (!validateRecordStatus.isOK() || validatedSize != dataSize) {
                // If status is not okay, dataSize is not reliable.
                if (!validateRecordStatus.isOK()) {
                    LOGV2_OPTIONS(
                        4835001,
                        {logv2::LogTruncation::Disabled},
                        "Document corruption details - Document validation failed with error",
                        "recordId"_attr = record->id,
                        "error"_attr = validateRecordStatus);
                } else {
                    LOGV2_OPTIONS(4835002,
                                  {logv2::LogTruncation::Disabled},
                                  "Document corruption details - Document validation failure; "
                                  "size mismatch",
                                  "recordId"_attr = record->id,
                                  "validatedBytes"_attr = validatedSize,
                                  "recordBytes"_attr = dataSize);
                }

                if (_validateState->fixErrors()) {
                    WriteUnitOfWork wunit(opCtx);
                    rs->deleteRecord(
                        opCtx, *shard_role_details::getRecoveryUnit(opCtx), record->id);
                    wunit.commit();
                    results.validateResults.setRepaired(true);
                    results.validateResults.addNumRemovedCorruptRecords(1);
                    results.validateResults.incrementNumRecords(-1);
                } else {
                    // If this is not set up to repair and remove the corrupt records, the error
                    // returned from record Validation should be logged if it exists.
                    if (!validateRecordStatus.isOK()) {
                        results.validateResults.addError(
                            maybeValidateRecordErrorMessage.value_or(kInvalidDocumentError));
                    }
                    numCorruptRecordsSizeBytes += record->id.memUsage();
                    if (numCorruptRecordsSizeBytes <= kMaxErrorSizeBytes) {
                        results.validateResults.addCorruptRecord(record->id);
                    } else {
                        results.validateResults.addWarning(
                            kNotEnoughSpaceToReportCorruptionWarning);
                    }
                    results.validateResults.incrementNumInvalidDocuments(1);
                }
            } else {
                // If the document is not corrupted, validate the document against this collection's
                // schema validator. Don't treat invalid documents as errors since documents can
                // bypass document validation when being inserted or updated.
                const auto [checkValidationResult, schemaValidationStatus] =
                    coll->checkValidation(opCtx, record->data.toBson());

                // Timeseries collections are a special case. The schema is required and all
                // violations will be logged as errors instead.
                const bool isTimeseries = coll->getTimeseriesOptions().has_value();

                switch (checkValidationResult.result) {
                    case Collection::SchemaValidationResult::kPass:
                        if (isTimeseries) {
                            // Timeseries documents checks cannot be run if schema validation fails.
                            const BSONObj recordBson = record->data.toBson();

                            // Checks for time-series collection consistency.
                            const auto timeseriesValidationResult =
                                collection_validation::validateTimeSeriesBucketRecord(
                                    opCtx,
                                    *_validateState,
                                    coll,
                                    recordBson,
                                    results.validateResults);
                            // This log id should be kept in sync with the associated warning
                            // messages that are returned to the client.
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
                                        "Document is not compliant with time-series "
                                        "specifications",
                                        logAttrs(coll->ns()),
                                        "recordId"_attr = record->id,
                                        "record"_attr = record->data.toBson(),
                                        "reason"_attr = timeseriesValidationResult.reason);
                                    results.validateResults.incrementNumNonCompliantDocuments(1);
                                    results.validateResults.addWarning(
                                        collection_validation::describeTimeseriesValidationResult(
                                            timeseriesValidationResult.result));
                                    break;

                                // All remaining result cases are errors
                                default:
                                    LOGV2_ERROR_OPTIONS(6698300,
                                                        {logv2::LogTruncation::Disabled},
                                                        "Document is not compliant with "
                                                        "time-series specifications",
                                                        logAttrs(coll->ns()),
                                                        "recordId"_attr = record->id,
                                                        "record"_attr = record->data.toBson(),
                                                        "reason"_attr =
                                                            timeseriesValidationResult.reason);
                                    results.validateResults.incrementNumNonCompliantDocuments(1);
                                    results.validateResults.addError(
                                        collection_validation::describeTimeseriesValidationResult(
                                            timeseriesValidationResult.result));
                            }
                            const auto containsMixedSchemaDataResponse =
                                coll->doesTimeseriesBucketsDocContainMixedSchemaData(recordBson);
                            if (!containsMixedSchemaDataResponse.isOK() &&
                                results.validateResults.addError(
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
                                    results.validateResults.addWarning(
                                        collection_validation::
                                            kExpectedMixedSchemaTimeseriesWarning)) {
                                    LOGV2_WARNING_OPTIONS(8469901,
                                                          {logv2::LogTruncation::Disabled},
                                                          collection_validation::
                                                              kExpectedMixedSchemaTimeseriesWarning,
                                                          logAttrs(coll->ns()),
                                                          "recordId"_attr = record->id);
                                } else if (!mixedSchemaAllowed &&
                                           results.validateResults.addError(
                                               collection_validation::
                                                   kUnexpectedMixedSchemaTimeseriesError)) {
                                    const auto& controlField =
                                        recordBson.getField(timeseries::kBucketControlFieldName)
                                            .Obj();
                                    const int count = controlField.getIntField(
                                        timeseries::kBucketControlCountFieldName);
                                    LOGV2_WARNING_OPTIONS(8469902,
                                                          {logv2::LogTruncation::Disabled},
                                                          collection_validation::
                                                              kUnexpectedMixedSchemaTimeseriesError,
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
                        // data-annotated strings to the set, since per-document data can
                        // greatly increase the number of unique strings stored; this set is not
                        // intended to scale with the number of documents. Document-specific
                        // data is logged.
                        results.validateResults.incrementNumNonCompliantDocuments(1);
                        const char* description =
                            _describeDocumentValidationResult(checkValidationResult);
                        if (isTimeseries) {
                            LOGV2_WARNING_OPTIONS(
                                11634800,
                                {logv2::LogTruncation::Disabled},
                                "Time-series bucket document is not compliant with "
                                "time-series specifications",
                                logAttrs(coll->ns()),
                                "recordId"_attr = record->id,
                                "collectionUUID"_attr = coll->uuid(),
                                "record"_attr = record->data.toBson(),
                                "reason"_attr = description);
                            results.validateResults.addError(description);
                        } else {
                            LOGV2_WARNING_OPTIONS(
                                5363500,
                                {logv2::LogTruncation::Disabled},
                                "Document is not compliant with the collection's schema",
                                logAttrs(coll->ns()),
                                "recordId"_attr = record->id,
                                "reason"_attr = description);
                            results.validateResults.addWarning(description);
                        }
                        break;
                    }
                }
            }

            prevRecordId = record->id;

            if (results.validateResults.getNumRecords().value() %
                        KeyStringIndexConsistency::kInterruptIntervalNumRecords ==
                    0 ||
                interruptIntervalNumBytes >= kInterruptIntervalNumBytes) {
                opCtx->checkForInterrupt();
                cursor->yield();

                if (interruptIntervalNumBytes >= kInterruptIntervalNumBytes) {
                    interruptIntervalNumBytes = 0;
                }
            }
        }

        if (_validateState->isCollHashValidation()) {
            results.validateResults.setCollectionHash(accumulatedBlock);
            if (computeXxh3Hash) {
                results.validateResults.setXxh3CollectionHash(accumulatedXxh3);
            }
            if (hashedCorruptRecord) {
                LOGV2_WARNING(13374600,
                              "Collection hashes cover records that failed validation, so they "
                              "hash those records' raw bytes rather than the documents they are "
                              "meant to hold",
                              logAttrs(_validateState->nss()));
            }
            if (revealHashedIds) {
                results.validateResults.setRevealedIds(std::move(revealedIds));
            }
        }
    } catch (const DBException& e) {
        results.status = e.toStatus();
    }

    return results;
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
        if (!_progress.get(lk) || !_progress.get(lk)->isActive()) {
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
