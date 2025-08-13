/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/temporary_record_store.h"
#include "mongo/platform/atomic_word.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mongo {

class IndexCatalogEntry;

/**
 * Records keys that have violated duplicate key constraints on unique indexes. The keys are backed
 * by a temporary table that is created and destroyed by this tracker.
 */
class DuplicateKeyTracker {
    DuplicateKeyTracker(const DuplicateKeyTracker&) = delete;
    DuplicateKeyTracker& operator=(const DuplicateKeyTracker&) = delete;

public:
    DuplicateKeyTracker(OperationContext* opCtx,
                        const IndexCatalogEntry* indexCatalogEntry,
                        StringData ident,
                        bool tableExists);

    /**
     * Keeps the temporary table for the duplicate key constraint violations.
     */
    void keepTemporaryTable();

    /**
     * Given a duplicate key, insert it into the key constraint table.
     */
    Status recordKey(OperationContext* opCtx,
                     const IndexCatalogEntry* indexCatalogEntry,
                     const key_string::View& key);

    /**
     * Returns boost::none if all previously recorded duplicate key constraint violations have been
     * resolved for the index. Returns duplicate key information if there are still duplicate key
     * constraint violations on the index.
     *
     * Must not be in a WriteUnitOfWork.
     */
    boost::optional<SortedDataInterface::DuplicateKey> checkConstraints(
        OperationContext* opCtx, const IndexCatalogEntry* indexCatalogEntry) const;

    std::string getTableIdent() const {
        return std::string{_keyConstraintsTable->rs()->getIdent()};
    }

private:
    AtomicWord<long long> _duplicateCounter{0};

    // This temporary record store is owned by the duplicate key tracker and dropped along with it.
    std::unique_ptr<TemporaryRecordStore> _keyConstraintsTable;
};

}  // namespace mongo
