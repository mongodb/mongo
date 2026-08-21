// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/validate/concurrent_progress_meter.h"
#include "mongo/db/validate/key_string_index_consistency.h"
#include "mongo/db/validate/validate_results.h"
#include "mongo/db/validate/validate_state.h"
#include "mongo/util/modules.h"
#include "mongo/util/progress_meter.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace mongo {

namespace collection_validation {
/**
 * Returns the number of additional characters (N) we are appending to each hashPrefix
 * that is passed in.
 *
 * The value of N is determined by the number of hashPrefixes provided. For each hashPrefix
 * provided, adding N characters means we will have potentially 16 ^ N buckets being created.
 * "Potentially" because some buckets may not be created if no documents hash to those buckets.
 *
 * Therefore this function ensures that we attach a number of characters so that we don't create
 * too many buckets, as creating too many buckets would mean exceeding the maximum BSON size
 * in our final response to the client.
 *
 * numHashPrefixes must be greater than 0, and hashPrefixLength must be less than or equal to
 * the size of a hash.
 */
size_t getNumberOfAdditionalCharactersForHashDrillDown(size_t numHashPrefixes,
                                                       size_t hashPrefixLength);
}  // namespace collection_validation

class IndexDescriptor;
class OperationContext;

/**
 * The cursor a record store traversal reads through, and with it the throttling and yielding policy
 * that traversal runs under. Injected so that the traversal itself does not have to know whether it
 * is scanning the whole record store or one slice of a parallel scan.
 */
class ValidateCursor {
public:
    /**
     * Opens the cursor for the thread that runs a traversal. Record store cursors are not thread
     * safe and so cannot be handed to a worker after the fact; the factory is instead invoked on
     * the thread that does the traversing, with that thread's own OperationContext.
     */
    using Factory = std::function<std::unique_ptr<ValidateCursor>(OperationContext*)>;

    virtual ~ValidateCursor() = default;

    /**
     * Positions the cursor at the start of the traversal, given its inclusive begin bound, and
     * returns the first record. Whether a record whose id does not match the bound exactly is
     * acceptable is left to the implementation, as it depends on where the bound came from.
     */
    [[nodiscard]] virtual boost::optional<Record> seek(const RecordId& recordId) = 0;

    /**
     * Advances the cursor and returns the record it lands on, or boost::none at the end of the
     * traversal. Marked [[nodiscard]] because the end-of-traversal signal is carried by the return
     * value alone; advancing without reading the record out is legitimate but has to say so with an
     * explicit cast to void.
     */
    [[nodiscard]] virtual boost::optional<Record> next() = 0;

    /**
     * Releases the storage snapshot and reacquires it, for implementations whose policy is to
     * yield; a no-op for those that do not. Throws if the cursor cannot be restored afterwards.
     */
    virtual void yield() = 0;
};

/**
 * The validate adaptor is used to keep track of collection and index consistency during a running
 * collection validation operation.
 */
class ValidateAdaptor {
public:
    ValidateAdaptor(OperationContext* opCtx, collection_validation::ValidateState* validateState)

        : _keyBasedIndexConsistency(opCtx, validateState), _validateState(validateState) {}

    struct ValidateRecordResult {
        Status status{Status::OK()};
        int dataSize{0};
        boost::optional<std::string> errorMessage{boost::none};
        // Whether the record conforms to BSON specifications, and whether it is a valid document
        // (e.g. within the BSON object size limit). Reported back rather than accumulated on the
        // adaptor so that the caller owns the counters.
        bool compliantDocument{true};
        bool validDocument{true};
    };
    /**
     * Validates the record data and traverses through its key set to keep track of the index
     * consistency. Returns the status from the record validation, and if a specific error was added
     * during record validation, returns that error as well.
     *
     * Errors and per-index results are recorded on 'results', and document keys are accumulated on
     * 'keyStringIndexConsistency'; neither the adaptor's own results nor its own index consistency
     * state are touched.
     */
    auto validateRecord(OperationContext* opCtx,
                        const Record& record,
                        ValidateResults& results,
                        KeyStringIndexConsistency& keyStringIndexConsistency,
                        std::span<const IndexCatalogEntry*> indexCatalogEntries,
                        ValidationVersion validationVersion = currentValidationVersion) const
        -> ValidateRecordResult;

    /**
     * Options describing the slice of the record store that a single traversal should cover.
     *
     * 'beginRecordId' is inclusive and 'endRecordId' is exclusive; a null 'endRecordId' means
     * "traverse to the end of the record store".
     */
    struct TraverseRecordStoreOptions {
        RecordId beginRecordId;
        RecordId endRecordId;

        /**
         * Opens the cursor this traversal reads through. A whole-record-store traversal passes a
         * factory returning the ValidateState's shared throttled cursor, preserving the throttling
         * and yielding behaviour of serial validation; a parallel slice passes one returning a
         * plain, unthrottled cursor of its own, since neither SeekableRecordThrottleCursor nor
         * DataThrottle is thread-safe.
         */
        ValidateCursor::Factory cursorFactory;
    };

    /**
     * The accumulated output of traversing one slice of the record store. Every field is
     * self-contained so that results for disjoint slices can be combined via
     * ValidateResults::merge() and KeyStringIndexConsistency::merge() (SERVER-128593) once the
     * traversal is parallelized in SERVER-127596.
     *
     * 'status' carries any exception thrown mid-traversal. The results accumulated up to that point
     * are still returned so that the caller can report partial progress before rethrowing.
     */
    struct TraverseRecordStoreResults {
        Status status = Status::OK();
        int64_t dataSizeTotal{0};
        ValidateResults validateResults;
        KeyStringIndexConsistency keyStringIndexConsistency;
    };

    /**
     * Traverses one slice of the record store, retrieving every record in the slice and going
     * through its document key set to keep track of the index consistency during a validation.
     *
     * This does not mutate any adaptor state: the traversal starts from copies of 'baseResults' and
     * 'baseConsistency' and returns them, mutated, to the caller. 'progress' is the one exception,
     * being purely a reporting side channel.
     */
    [[nodiscard]] auto traverseRecordStoreImpl(OperationContext* opCtx,
                                               const ValidateResults& baseResults,
                                               const KeyStringIndexConsistency& baseConsistency,
                                               TraverseRecordStoreOptions opts,
                                               ConcurrentProgressMeterHolder& progress,
                                               ValidationVersion validationVersion) const
        -> TraverseRecordStoreResults;

    /**
     * Traverses the record store to retrieve every record and go through its document key
     * set to keep track of the index consistency during a validation. Runs the collection-level
     * checks (fast count and fast size) that require the totals from a complete traversal.
     */
    void traverseRecordStore(OperationContext* opCtx,
                             ValidateResults& results,
                             ValidationVersion validationVersion);
    /**
     * Computes the hash of the collection's local catalog idents and sets it in 'results'.
     **/
    void computeMetadataHash(OperationContext* opCtx,
                             const CollectionPtr& coll,
                             ValidateResults* results);

    /**
     * For a given set of hash prefixes, outputs an order independent hash of all the documents
     * whose _id hashes to each hash prefix.
     **/
    void hashDrillDown(OperationContext* opCtx, ValidateResults* results);

    /**
     * Traverses the index getting index entries to validate them and keep track of the index keys
     * for index consistency.
     */
    void traverseIndex(OperationContext* opCtx,
                       const IndexCatalogEntry* index,
                       int64_t* numTraversedKeys,
                       ValidateResults* results);

    /**
     * Validates that the number of document keys matches the number of index keys previously
     * traversed in traverseIndex().
     */
    void validateIndexKeyCount(OperationContext* opCtx,
                               const IndexCatalogEntry* index,
                               IndexValidateResults& results);

    /**
     * Informs the index consistency objects that we're advancing to the second phase of index
     * validation.
     */
    void setSecondPhase();

    /**
     * Sets up the index consistency objects to limit memory usage in the second phase of index
     * validation. Returns whether the memory limit is sufficient to report at least one index entry
     * inconsistency and continue with the second phase of validation.
     */
    bool limitMemoryUsageForSecondPhase(ValidateResults* result);

    /**
     * Returns true if the underlying index consistency objects have entry mismatches.
     */
    bool haveEntryMismatch() const;

    /**
     * If repair mode enabled, try inserting _missingIndexEntries into indexes.
     */
    void repairIndexEntries(OperationContext* opCtx, ValidateResults* results);

    /**
     * Records the errors gathered from the second phase of index validation into the provided
     * ValidateResultsMap and ValidateResults.
     */
    void addIndexEntryErrors(OperationContext* opCtx, ValidateResults* results);

private:
    KeyStringIndexConsistency _keyBasedIndexConsistency;
    collection_validation::ValidateState* _validateState;

    // Saves the record count from the record store traversal to be used later to validate the index
    // entries count. Reset every time traverseRecordStore() is called.
    long long _numRecords = 0;

    ConcurrentProgressMeterHolder _progress;
};
}  // namespace mongo
