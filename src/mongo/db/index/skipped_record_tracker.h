/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/temporary_record_store.h"
#include "mongo/platform/atomic_word.h"

namespace mongo {

class IndexCatalogEntry;

/**
 * Records keys that have violated index key constraints. The keys are backed by a temporary table
 * that is created and destroyed by this tracker.
 */
class SkippedRecordTracker {
    SkippedRecordTracker(const SkippedRecordTracker&) = delete;

public:
    explicit SkippedRecordTracker(IndexCatalogEntry* indexCatalogEntry);
    SkippedRecordTracker(OperationContext* opCtx,
                         IndexCatalogEntry* indexCatalogEntry,
                         boost::optional<StringData> ident);

    /**
     * Records a RecordId that was unable to be indexed due to a key generation error. At the
     * conclusion of the build, the key generation and insertion into the index should be attempted
     * again by calling 'retrySkippedRecords'.
     */
    void record(OperationContext* opCtx, const RecordId& recordId);

    /**
     * Deletes or keeps the temporary table managed by this tracker. This call is required, and is
     * a no-op when the table is empty or has not yet been initialized.
     */
    void finalizeTemporaryTable(OperationContext* opCtx,
                                TemporaryRecordStore::FinalizationAction action);

    /**
     * Returns true if the temporary table is empty.
     */
    bool areAllRecordsApplied(OperationContext* opCtx) const;

    /**
     * Attempts to generates keys for each skipped record and insert into the index. Returns OK if
     * all records were either indexed or no longer exist.
     */
    Status retrySkippedRecords(OperationContext* opCtx, const CollectionPtr& collection);

    boost::optional<std::string> getTableIdent() const {
        return _skippedRecordsTable ? boost::make_optional(_skippedRecordsTable->rs()->getIdent())
                                    : boost::none;
    }

private:
    IndexCatalogEntry* _indexCatalogEntry;

    // This temporary record store is owned by the duplicate key tracker and should be dropped or
    // kept along with it with a call to finalizeTemporaryTable().
    std::unique_ptr<TemporaryRecordStore> _skippedRecordsTable;

    AtomicWord<std::uint32_t> _skippedRecordCounter{0};
};

}  // namespace mongo
