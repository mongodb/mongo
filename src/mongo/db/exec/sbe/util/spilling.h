// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/util/builder.h"
#include "mongo/db/exec/sbe/values/row.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/spill_table.h"
#include "mongo/util/modules.h"

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
std::pair<RecordId, key_string::TypeBits> encodeKeyString(
    key_string::Builder&, const value::FixedSizeRow<1 /* N */>& value);

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
    // This is currently only used with FixedSizeRow<1>
    boost::optional<value::FixedSizeRow<1 /* N */>> readFromRecordStore(OperationContext* opCtx,
                                                                        const RecordId& rid);

    bool findRecord(OperationContext* opCtx, const RecordId& loc, RecordData* out);

    auto getCursor(OperationContext* opCtx) {
        return _spillTable->getCursor(opCtx);
    }

    void resetCursor(OperationContext* opCtx, std::unique_ptr<SpillTable::Cursor>& cursor) {
        cursor.reset();
    }

    auto saveCursor(OperationContext* opCtx, std::unique_ptr<SpillTable::Cursor>& cursor) {
        cursor->save();
        cursor->detachFromOperationContext();
    }

    auto restoreCursor(OperationContext* opCtx, std::unique_ptr<SpillTable::Cursor>& cursor) {
        cursor->reattachToOperationContext(opCtx);
        return cursor->restore();
    }

    int64_t storageSize(OperationContext* opCtx);

    void updateSpillStorageStatsForOperation(OperationContext* opCtx);

private:
    std::unique_ptr<SpillTable> _spillTable;

    size_t _counter{0};
};
}  // namespace sbe
}  // namespace mongo
