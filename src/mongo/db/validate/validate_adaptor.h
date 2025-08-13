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

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/validate/index_consistency.h"
#include "mongo/db/validate/validate_results.h"
#include "mongo/db/validate/validate_state.h"
#include "mongo/util/progress_meter.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace mongo {

class IndexDescriptor;
class OperationContext;

/**
 * The validate adaptor is used to keep track of collection and index consistency during a running
 * collection validation operation.
 */
class ValidateAdaptor {
public:
    ValidateAdaptor(OperationContext* opCtx, CollectionValidation::ValidateState* validateState)

        : _keyBasedIndexConsistency(opCtx, validateState), _validateState(validateState) {}

    /**
     * Validates the record data and traverses through its key set to keep track of the
     * index consistency.
     */
    virtual Status validateRecord(OperationContext* opCtx,
                                  const RecordId& recordId,
                                  const RecordData& record,
                                  long long* nNonCompliantDocuments,
                                  size_t* dataSize,
                                  ValidateResults* results,
                                  ValidationVersion validationVersion = currentValidationVersion);
    /**
     * Traverses the record store to retrieve every record and go through its document key
     * set to keep track of the index consistency during a validation.
     */
    void traverseRecordStore(OperationContext* opCtx,
                             ValidateResults* results,
                             ValidationVersion validationVersion);

    /**
     * Traverses the index getting index entries to validate them and keep track of the index keys
     * for index consistency.
     */
    void traverseIndex(OperationContext* opCtx,
                       const IndexCatalogEntry* index,
                       int64_t* numTraversedKeys,
                       ValidateResults* results);

    /**
     * Traverses a record on the underlying index consistency objects.
     */
    void traverseRecord(OperationContext* opCtx,
                        const CollectionPtr& coll,
                        const IndexCatalogEntry* index,
                        const RecordId& recordId,
                        const BSONObj& record,
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
    CollectionValidation::ValidateState* _validateState;

    // Saves the record count from the record store traversal to be used later to validate the index
    // entries count. Reset every time traverseRecordStore() is called.
    long long _numRecords = 0;

    // For reporting progress during record store and index traversal.
    ProgressMeterHolder _progress;
};
}  // namespace mongo
