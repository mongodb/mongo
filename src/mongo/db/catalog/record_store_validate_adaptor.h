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

#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_consistency.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/record_store.h"

namespace mongo {

class IndexConsistency;

enum ValidateCmdLevel : int { kValidateNormal = 0x01, kValidateFull = 0x02 };

namespace {

using ValidateResultsMap = std::map<std::string, ValidateResults>;

}  // namespace

/**
 * The record store validate adaptor is used to keep track of the index consistency during
 * a validation that's running.
 */
class RecordStoreValidateAdaptor : public ValidateAdaptor {
public:
    RecordStoreValidateAdaptor(OperationContext* opCtx,
                               IndexConsistency* indexConsistency,
                               ValidateCmdLevel level,
                               IndexCatalog* ic,
                               ValidateResultsMap* irm)

        : _opCtx(opCtx),
          _indexConsistency(indexConsistency),
          _level(level),
          _indexCatalog(ic),
          _indexNsResultsMap(irm) {}

    /**
     * Validates the BSON object and traverses through its key set to keep track of the
     * index consistency.
     */
    virtual Status validate(const RecordId& recordId, const RecordData& record, size_t* dataSize);

    /**
     * Traverses the index getting index entriess to validate them and keep track of the index keys
     * for index consistency.
     */
    void traverseIndex(const IndexAccessMethod* iam,
                       const IndexDescriptor* descriptor,
                       ValidateResults* results,
                       int64_t* numTraversedKeys);

    /**
     * Traverses the record store to retrieve every record and go through its document key
     * set to keep track of the index consistency during a validation.
     */
    void traverseRecordStore(RecordStore* recordStore,
                             ValidateResults* results,
                             BSONObjBuilder* output);

    /**
     * Validate that the number of document keys matches the number of index keys.
     */
    void validateIndexKeyCount(const IndexDescriptor* idx,
                               int64_t numRecs,
                               ValidateResults& results);

private:
    OperationContext* _opCtx;
    IndexConsistency* _indexConsistency;
    ValidateCmdLevel _level;
    IndexCatalog* _indexCatalog;
    ValidateResultsMap* _indexNsResultsMap;
};
}  // namespace
