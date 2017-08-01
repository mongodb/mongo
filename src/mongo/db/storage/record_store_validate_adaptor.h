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
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/record_store.h"

namespace mongo {

namespace {
const uint32_t kKeyCountTableSize = 1U << 22;

using IndexKeyCountTable = std::array<uint64_t, kKeyCountTableSize>;
using ValidateResultsMap = std::map<std::string, ValidateResults>;
}

/**
 * The record store validate adaptor is used to keep track of the index consistency during
 * a validation that's running.
 */
class RecordStoreValidateAdaptor : public ValidateAdaptor {
public:
    RecordStoreValidateAdaptor(OperationContext* opCtx,
                               ValidateCmdLevel level,
                               IndexCatalog* ic,
                               ValidateResultsMap* irm)

        : _ikc(stdx::make_unique<IndexKeyCountTable>()),
          _opCtx(opCtx),
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
     * Validate that the number of document keys matches the number of index keys.
     */
    void validateIndexKeyCount(IndexDescriptor* idx, int64_t numRecs, ValidateResults& results);

    /**
     * Returns true if there are too many index entries, otherwise return false.
     */
    bool tooManyIndexEntries() const {
        return _indexKeyCountTableNumEntries != 0;
    }

    /**
     * Returns true if there are too few index entries, which happens when a document doesn't have
     * and index entry, otherwise return false.
     */
    bool tooFewIndexEntries() const {
        return _hasDocWithoutIndexEntry;
    }


private:
    std::map<std::string, int64_t> _longKeys;
    std::map<std::string, int64_t> _keyCounts;
    std::unique_ptr<IndexKeyCountTable> _ikc;

    uint32_t _indexKeyCountTableNumEntries = 0;
    bool _hasDocWithoutIndexEntry = false;

    const int IndexKeyMaxSize = 1024;  // this goes away with SERVER-3372

    OperationContext* _opCtx;  // Not owned.
    ValidateCmdLevel _level;
    IndexCatalog* _indexCatalog;             // Not owned.
    ValidateResultsMap* _indexNsResultsMap;  // Not owned.
};
}  // namespace
