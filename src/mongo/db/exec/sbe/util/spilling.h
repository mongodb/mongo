/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/bson/util/builder.h"
#include "mongo/db/exec/sbe/values/row.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/spill_table.h"

#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace sbe {

// Proactively assert that this operation can safely write before hitting an assertion in the
// storage engine. We can safely write if we are enforcing prepare conflicts by blocking or if we
// are ignoring prepare conflicts and explicitly allowing writes. Ignoring prepare conflicts
// without allowing writes will cause this operation to fail in the storage engine.
void assertIgnorePrepareConflictsBehavior(OperationContext* opCtx);

// Encode key as a RecordId and TypeBits.
std::pair<RecordId, key_string::TypeBits> encodeKeyString(key_string::Builder&,
                                                          const value::MaterializedRow& value);

// Reconstructs the KeyString carried in RecordId using 'typeBits'.
key_string::Value decodeKeyString(const RecordId& rid, key_string::TypeBits typeBits);

/**
 * SpillingStore is a wrapper around a temporary record store than maintains its own transaction as
 * we do not want to intermingle operations running in the main query with spill reads and writes.
 */
class SpillingStore {
public:
    SpillingStore(OperationContext* opCtx, KeyFormat format = KeyFormat::String);
    ~SpillingStore();

    /**
     * When a collator is provided, the key is encoded using the collator before being converted to
     * a record id. In this case, it is not possible to recover the key from the record id, thus we
     * need to store the original value of the key as well.
     */
    int upsertToRecordStore(OperationContext* opCtx,
                            const RecordId& recordKey,
                            const value::MaterializedRow& key,
                            const value::MaterializedRow& val,
                            bool update);
    /**
     * Inserts or updates a key/value into 'rs'. The 'update' flag controls whether or not an update
     * will be performed. If a key/value pair is inserted into the 'rs' that already exists and
     * 'update' is false, this function will tassert.
     *
     * Returns the size of the new record in bytes, including the record id and value portions.
     */
    int upsertToRecordStore(OperationContext* opCtx,
                            const RecordId& key,
                            const value::MaterializedRow& val,
                            const key_string::TypeBits& typeBits,
                            bool update);
    int upsertToRecordStore(OperationContext* opCtx,
                            const RecordId& key,
                            BufBuilder& buf,
                            const key_string::TypeBits& typeBits,  // recover type of value.
                            bool update);
    int upsertToRecordStore(OperationContext* opCtx,
                            const RecordId& key,
                            BufBuilder& buf,
                            bool update);


    Status insertRecords(OperationContext* opCtx, std::vector<Record>* inOutRecords);

    // Reads a materialized row from the record store.
    boost::optional<value::MaterializedRow> readFromRecordStore(OperationContext* opCtx,
                                                                const RecordId& rid);

    bool findRecord(OperationContext* opCtx, const RecordId& loc, RecordData* out);

    auto getCursor(OperationContext* opCtx) {
        switchToSpilling(opCtx);
        ON_BLOCK_EXIT([&] { switchToOriginal(opCtx); });
        return _spillTable->getCursor(opCtx);
    }

    void resetCursor(OperationContext* opCtx, std::unique_ptr<SpillTable::Cursor>& cursor) {
        switchToSpilling(opCtx);
        ON_BLOCK_EXIT([&] { switchToOriginal(opCtx); });
        cursor.reset();
    }

    auto saveCursor(OperationContext* opCtx, std::unique_ptr<SpillTable::Cursor>& cursor) {
        switchToSpilling(opCtx);
        ON_BLOCK_EXIT([&] { switchToOriginal(opCtx); });

        cursor->save();
        cursor->detachFromOperationContext();
    }

    auto restoreCursor(OperationContext* opCtx, std::unique_ptr<SpillTable::Cursor>& cursor) {
        switchToSpilling(opCtx);
        ON_BLOCK_EXIT([&] { switchToOriginal(opCtx); });

        cursor->reattachToOperationContext(opCtx);
        return cursor->restore(*shard_role_details::getRecoveryUnit(opCtx));
    }

    int64_t storageSize(OperationContext* opCtx);

    void saveState();
    void restoreState();

    void updateSpillStorageStatsForOperation(OperationContext* opCtx);

private:
    void switchToSpilling(OperationContext* opCtx);
    void switchToOriginal(OperationContext* opCtx);

    std::unique_ptr<SpillTable> _spillTable;

    std::unique_ptr<RecoveryUnit> _originalUnit;
    WriteUnitOfWork::RecoveryUnitState _originalState;

    std::unique_ptr<RecoveryUnit> _spillingUnit;
    WriteUnitOfWork::RecoveryUnitState _spillingState;

    size_t _counter{0};
};
}  // namespace sbe
}  // namespace mongo
