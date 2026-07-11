// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <string_view>

namespace mongo {
/**
 * A wrapper around a record store which creates the table when required.
 */
class [[MONGO_MOD_PUBLIC]] LazyRecordStore {
public:
    enum class CreateMode {
        /**
         * Create the backing table immediately.
         */
        immediate,
        /**
         * Create the backing table on first use.
         */
        deferred,
        /**
         * Reuse an existing backing table. The table must exist.
         */
        openExisting,
    };

    /**
     * Creates a LazyRecordStore and possibly creates or opens the internal table, depending on the
     * create mode given.
     *
     * If called inside an active WriteUnitOfWork with CreateMode::immediate, the WUOW must be
     * committed or rolled back before this LazyRecordStore is destroyed.
     */
    LazyRecordStore(OperationContext* opCtx, std::string_view ident, CreateMode createMode);
    ~LazyRecordStore();

    /**
     * Creates the internal table if needed and then returns the record store.
     *
     * If called inside an active WriteUnitOfWork, the WUOW must be committed or rolled back before
     * this LazyRecordStore is destroyed.
     */
    RecordStore& getOrCreateTable(OperationContext* opCtx);

    /**
     * Returns true if the backing table has been created or opened.
     */
    bool tableExists() const;

    /**
     * Returns a reference to the record store. Throws if the table has not been created.
     */
    RecordStore& getTableOrThrow() const;

    /**
     * Drops the table via the reaper. If the table was never created (deferred mode), this is a
     * no-op. After calling this, tableExists() will return false, and getOrCreateTable() will
     * re-create the table on next use.
     * Requires a minimum timestamp to be provided, which acts as a lower bound for the drop reaper,
     * ensuring the table will stay alive until the oldest timestamp has advanced past the drop
     * time.
     */
    void drop(OperationContext* opCtx, StorageEngine::DropTime dropTime);

    /**
     * Immediately creates a table which can later be used with CreateMode::openExisting.
     */
    static void createTable(OperationContext* opCtx, std::string_view ident);

private:
    // Set to true while _tableOrIdent is storing a RecordStore whose creating WUOW has not been
    // committed yet. Used to fail noisily if the required lifetime rules are not followed.
    bool _hasPendingCreation = false;

    std::variant<std::string, std::unique_ptr<RecordStore>> _tableOrIdent;

    static std::unique_ptr<RecordStore> _createRecordStore(OperationContext* opCtx,
                                                           std::string_view ident,
                                                           LazyRecordStore* lrs);
};

}  // namespace mongo
