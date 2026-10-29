// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/storage/execution_context.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_error_util.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_prepare_conflict.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/util/modules.h"

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
