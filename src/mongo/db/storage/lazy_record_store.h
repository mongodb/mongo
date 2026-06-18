/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
class MONGO_MOD_PUBLIC LazyRecordStore {
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
