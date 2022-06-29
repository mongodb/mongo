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

#include <wiredtiger.h>

#include "mongo/db/operation_context.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"

namespace mongo {

/**
 * This is an object wrapper for WT_CURSOR. It obtains a cursor from the WiredTigerSession and is
 * responsible for returning or closing the cursor when destructed.
 */
class WiredTigerCursor {
public:
    /**
     * If 'allowOverwrite' is true, insert operations will not return an error if the record
     * already exists, and update/remove operations will not return error if the record does not
     * exist.
     */
    WiredTigerCursor(const std::string& uri,
                     uint64_t tableID,
                     bool allowOverwrite,
                     OperationContext* opCtx);

    ~WiredTigerCursor();

    WT_CURSOR* get() const {
        return _cursor;
    }

    WT_CURSOR* operator->() const {
        return get();
    }

    WiredTigerSession* getSession() {
        return _session;
    }

    void reset();

    void assertInActiveTxn() const {
        _ru->assertInActiveTxn();
    }

protected:
    uint64_t _tableID;
    WiredTigerRecoveryUnit* _ru;
    WiredTigerSession* _session;
    std::string _config;

    WT_CURSOR* _cursor = nullptr;  // Owned
};

/**
 * An owning object wrapper for a WT_SESSION and WT_CURSOR configured for bulk loading when
 * possible. The cursor is created and closed independently of the cursor cache, which does not
 * store bulk cursors. It uses its own session to avoid hijacking an existing transaction in the
 * current session.
 */
class WiredTigerBulkLoadCursor {
public:
    WiredTigerBulkLoadCursor(const std::string& indexUri, OperationContext* opCtx);

    ~WiredTigerBulkLoadCursor() {
        _cursor->close(_cursor);
    }

    WT_CURSOR* get() const {
        return _cursor;
    }

    WT_CURSOR* operator->() const {
        return get();
    }

private:
    UniqueWiredTigerSession const _session;
    WT_CURSOR* _cursor = nullptr;  // Owned
};
}  // namespace mongo
