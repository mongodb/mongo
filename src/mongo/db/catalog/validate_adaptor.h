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

#include "mongo/db/catalog/validate_state.h"
#include "mongo/util/progress_meter.h"

namespace mongo {

class IndexConsistency;
class IndexDescriptor;
class OperationContext;

/**
 * The validate adaptor is used to keep track of collection and index consistency during a running
 * collection validation operation.
 */
class ValidateAdaptor {
public:
    ValidateAdaptor(IndexConsistency* indexConsistency,
                    CollectionValidation::ValidateState* validateState)

        : _indexConsistency(indexConsistency), _validateState(validateState) {}

    /**
     * Validates the record data and traverses through its key set to keep track of the
     * index consistency.
     */
    virtual Status validateRecord(OperationContext* opCtx,
                                  const RecordId& recordId,
                                  const RecordData& record,
                                  long long* nNonCompliantDocuments,
                                  size_t* dataSize,
                                  ValidateResults* results);

    /**
     * Traverses the index getting index entries to validate them and keep track of the index keys
     * for index consistency.
     */
    void traverseIndex(OperationContext* opCtx,
                       const IndexCatalogEntry* index,
                       int64_t* numTraversedKeys,
                       ValidateResults* results);

    /**
     * Traverses the record store to retrieve every record and go through its document key
     * set to keep track of the index consistency during a validation.
     */
    void traverseRecordStore(OperationContext* opCtx,
                             ValidateResults* results,
                             BSONObjBuilder* output);

    /**
     * Validates that the number of document keys matches the number of index keys previously
     * traversed in traverseIndex().
     */
    void validateIndexKeyCount(OperationContext* opCtx,
                               const IndexCatalogEntry* index,
                               IndexValidateResults& results);

private:
    IndexConsistency* _indexConsistency;
    CollectionValidation::ValidateState* _validateState;

    // Saves the record count from the record store traversal to be used later to validate the index
    // entries count. Reset every time traverseRecordStore() is called.
    long long _numRecords = 0;

    // For reporting progress during record store and index traversal.
    ProgressMeterHolder _progress;

    // The total number of index keys is stored during the first validation phase, since this
    // count may change during a second phase.
    uint64_t _totalIndexKeys = 0;
};
}  // namespace mongo
