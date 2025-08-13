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

#include <boost/container/small_vector.hpp>
// IWYU pragma: no_include "boost/intrusive/detail/iterator.hpp"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/temporary_record_store.h"
#include "mongo/platform/atomic_word.h"

#include <cstdint>
#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/type_traits/decay.hpp>

namespace mongo {

class IndexCatalogEntry;

/**
 * Records keys that have violated index key constraints. The keys are backed by a temporary table
 * that is created and destroyed by this tracker.
 */
class SkippedRecordTracker {
    SkippedRecordTracker(const SkippedRecordTracker&) = delete;

public:
    enum class RetrySkippedRecordMode {
        // Retry key generation but do not update the index or remove the records from the tracker.
        kKeyGeneration,
        // Retry key generation and update the index with the new keys, removing the retried records
        // from the tracker.
        kKeyGenerationAndInsertion
    };

    SkippedRecordTracker(OperationContext* opCtx,
                         StringData skippedRecordsTrackerIdent,
                         bool tableExists);

    /**
     * Records a RecordId that was unable to be indexed due to a key generation error. At the
     * conclusion of the build, the key generation and insertion into the index should be attempted
     * again by calling 'retrySkippedRecords'.
     */
    void record(OperationContext* opCtx, const RecordId& recordId);

    /**
     * Keeps the temporary table managed by this tracker. This is a no-op when the table is empty or
     * has not yet been initialized.
     */
    void keepTemporaryTable();

    /**
     * Returns true if the temporary table is empty.
     */
    bool areAllRecordsApplied(OperationContext* opCtx) const;

    /**
     * By default, attempts to generate keys for each skipped record and insert into the index.
     * Returns OK if all records were either indexed or no longer exist.
     *
     * The behaviour can be modified by specifying a RetrySkippedRecordMode.
     */
    Status retrySkippedRecords(
        OperationContext* opCtx,
        const CollectionPtr& collection,
        const IndexCatalogEntry* indexCatalogEntry,
        RetrySkippedRecordMode mode = RetrySkippedRecordMode::kKeyGenerationAndInsertion);

    boost::optional<std::string> getTableIdent() const {
        return _skippedRecordsTable
            ? boost::make_optional(std::string{_skippedRecordsTable->rs()->getIdent()})
            : boost::none;
    }

    boost::optional<MultikeyPaths> getMultikeyPaths() const {
        return _multikeyPaths;
    }

private:
    std::string _ident;

    // This temporary record store is owned by the duplicate key tracker.
    std::unique_ptr<TemporaryRecordStore> _skippedRecordsTable;

    AtomicWord<std::uint32_t> _skippedRecordCounter{0};

    boost::optional<MultikeyPaths> _multikeyPaths;
};

}  // namespace mongo
