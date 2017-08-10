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

#pragma once

#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_consistency.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/record_store.h"

namespace mongo {

class IndexConsistency;

namespace {

using ValidateResultsMap = std::map<std::string, ValidateResults>;
}

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
                             ValidateCmdLevel level,
                             ValidateResults* results,
                             BSONObjBuilder* output);

    /**
     * Validate that the number of document keys matches the number of index keys.
     */
    void validateIndexKeyCount(IndexDescriptor* idx, int64_t numRecs, ValidateResults& results);

private:
    OperationContext* _opCtx;             // Not owned.
    IndexConsistency* _indexConsistency;  // Not owned.
    ValidateCmdLevel _level;
    IndexCatalog* _indexCatalog;             // Not owned.
    ValidateResultsMap* _indexNsResultsMap;  // Not owned.
};
}  // namespace
