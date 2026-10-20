// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/validate/validate_options.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

#include <boost/optional.hpp>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

class NamespaceString;
class OperationContext;
class Collection;
class CollectionPtr;
class BSONObjBuilder;
class Status;
class ValidateResults;

namespace collection_validation {

/**
 * Timings describing the cost of each stage of a collection validation. A field is only populated
 * if validation reached the corresponding stage. These vary between otherwise identical
 * validations, so they are logged rather than kept in ValidateResults.
 */
struct [[MONGO_MOD_FILE_PRIVATE]] ValidationMetrics {
    // Whether the collection is a time-series collection. Only known once the collection is found.
    boost::optional<bool> isTimeseries;
    // Wall-clock time spent in collection_validation::validate().
    boost::optional<Microseconds> totalDuration;
    // Time spent validating the internal structure of the record store. Only full validation runs
    // WT_SESSION::verify, so this is much smaller otherwise.
    boost::optional<Microseconds> recordStoreValidateDuration;
    // Time spent validating the internal structure of all indexes. Only full validation runs
    // WT_SESSION::verify, so this is much smaller otherwise.
    boost::optional<Microseconds> indexValidateDuration;
    // Time spent in the first phase of index consistency checks, which covers the record store and
    // index traversals.
    boost::optional<Microseconds> phase1Duration;
    boost::optional<Microseconds> recordStoreTraversalDuration;
    boost::optional<Microseconds> indexTraversalDuration;
    // Time spent in the second phase, which only runs when index inconsistencies are found.
    boost::optional<Microseconds> phase2Duration;

    /**
     * Serializes the metrics along with the bytes traversed, as recorded in 'results'.
     */
    BSONObj toBSON(const ValidateResults& results) const;
};

/**
 * Checks if 'hashPrefixes' contains valid hash strings. Throws if any is invalid.
 * When 'equalLength' is true, also checks all hash strings have the same length.
 */
[[MONGO_MOD_FILE_PRIVATE]] void validateHashes(const std::vector<std::string>& hashPrefixes,
                                               bool equalLength);

/**
 * The target number of records per record store slice to use when splitting a collection for
 * parallel traversal, or boost::none when parallel validation is not enabled.
 */
boost::optional<int64_t> getTargetRecordsPerRecordStoreSlice();

/**
 * Whether a validation given 'targetRecordsPerSlice' may split the record store into slices for
 * parallel traversal.
 */
bool isRecordStoreSlicingEnabled(const boost::optional<int64_t>& targetRecordsPerSlice);

/**
 * Parses and checks the command object and returns a 'ValidationOptions' object used for collection
 * validation.
 * Optionally skips parsing 'atClusterTime' for unreplicated collections, which is desired with
 * modal validation usage.
 * When 'enableSizeStats' is true and the validation is a collHash validation, a storage size
 * summary is accumulated and logged. This is reserved for the offline startup '--validate' path
 * (fleet validation); it is intentionally not exposed as a command option.
 */
ValidationOptions parseValidateOptions(OperationContext* opCtx,
                                       NamespaceString nss,
                                       const BSONObj& cmdObj,
                                       bool skipAtClusterTime = false,
                                       bool enableSizeStats = false);

/**
 * Expects the caller to hold no locks.
 *
 * @return OK if the validate run successfully
 *         OK will be returned even if corruption is found
 *         details will be in 'results'.
 */
Status validate(OperationContext* opCtx,
                const NamespaceString& nss,
                ValidationOptions options,
                ValidateResults& results);

/**
 * Checks whether a failpoint has been hit in the above validate() code..
 */
[[MONGO_MOD_FILE_PRIVATE]] bool getIsValidationPausedForTest();

}  // namespace collection_validation
}  // namespace mongo
