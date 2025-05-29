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

#include "mongo/db/operation_context.h"
#include "mongo/db/storage/execution_context.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_error_util.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_prepare_conflict.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"

namespace mongo {
/**
 * Common logic for a cursor over any WiredTiger index.
 */
class WiredTigerIndexCursorGeneric {
public:
    WiredTigerIndexCursorGeneric(OperationContext* opCtx, bool forward)
        : _opCtx(opCtx), _forward(forward) {}
    virtual ~WiredTigerIndexCursorGeneric() = default;

    void resetCursor() {
        if (_cursor) {
            WT_CURSOR* wtCur = _cursor->get();
            invariantWTOK(WT_READ_CHECK(wtCur->reset(wtCur)), wtCur->session);
        }
    }

    void detachFromOperationContext() {
        _opCtx = nullptr;

        if (!_saveStorageCursorOnDetachFromOperationContext) {
            _cursor = boost::none;
        }
    }

    void reattachToOperationContext(OperationContext* opCtx) {
        _opCtx = opCtx;
        // _cursor recreated in restore() to avoid risk of WT_ROLLBACK issues.
    }

    void setSaveStorageCursorOnDetachFromOperationContext(bool saveCursor) {
        _saveStorageCursorOnDetachFromOperationContext = saveCursor;
    }

protected:
    /**
     * Returns false if and only if the cursor advanced to EOF.
     */
    [[nodiscard]] bool advanceWTCursor(RecoveryUnit& ru) {
        WT_CURSOR* c = _cursor->get();
        int ret = wiredTigerPrepareConflictRetry(
            *_opCtx, StorageExecutionContext::get(_opCtx)->getPrepareConflictTracker(), ru, [&] {
                return _forward ? c->next(c) : c->prev(c);
            });
        if (ret == WT_NOTFOUND) {
            return false;
        }
        invariantWTOK(ret, c->session);
        return true;
    }

    void getKey(WT_CURSOR* cursor, WT_ITEM* key) {
        invariantWTOK(cursor->get_key(cursor, key), cursor->session);
    }

    void getKeyValue(WT_CURSOR* cursor, WT_ITEM* key, WT_ITEM* value) {
        invariantWTOK(cursor->get_raw_key_value(cursor, key, value), cursor->session);
    }

    OperationContext* _opCtx;
    const bool _forward;
    boost::optional<WiredTigerCursor> _cursor;

    bool _saveStorageCursorOnDetachFromOperationContext = false;
};
}  // namespace mongo
