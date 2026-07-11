// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/storage/wiredtiger/wiredtiger_managed_session.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <string>
#include <string_view>

#include <wiredtiger.h>

namespace mongo {

/**
 * This is an object wrapper for WT_CURSOR. It obtains a cursor from the WiredTigerSession and is
 * responsible for returning or closing the cursor when destructed.
 */
class WiredTigerCursor {
public:
    struct Params {
        // The table id this cursor will operate on.
        uint64_t tableID{0};
        // When 'true', data read from disk should not be kept in the storage engine cache.
        bool readOnce{false};
        bool allowOverwrite{false};
        bool random{false};
    };

    /**
     * If 'allowOverwrite' is true, insert operations will not return an error if the record
     * already exists, and update/remove operations will not return error if the record does not
     * exist.
     *
     * If 'random' is true, every next calls will yield records in a random order.
     */
    WiredTigerCursor(Params params, std::string_view uri, WiredTigerSession& session);

    // Prevent duplication of the logical owned-ness of the cursors via move or copy.
    WiredTigerCursor(WiredTigerCursor&&) = delete;
    WiredTigerCursor(const WiredTigerCursor&) = delete;
    WiredTigerCursor& operator=(WiredTigerCursor&&) = delete;
    WiredTigerCursor& operator=(const WiredTigerCursor&) = delete;

    ~WiredTigerCursor();

    WT_CURSOR* get() const {
        return _cursor;
    }

    WT_CURSOR* operator->() const {
        return get();
    }

    WiredTigerSession* getSession() {
        return &_session;
    }

protected:
    uint64_t _tableID;
    WiredTigerSession& _session;
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
    WiredTigerBulkLoadCursor(OperationContext* opCtx,
                             WiredTigerSession& outerSession,
                             const std::string& indexUri);

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
    WiredTigerManagedSession const _session;
    WT_CURSOR* _cursor = nullptr;  // Owned
};

/**
 * An owning object wrapper for a WT_CURSOR configured to return prepared_id for all unresolved
 * prepared transactions in the current checkpoint that have not yet been claimed. It is used to
 * reconstruct prepared transactions after loading the initial checkpoint as a standby.
 */
class WiredTigerPrepareCursor {
public:
    // The caller must ensure that 'session' remains alive and valid for the entire lifetime
    // of this cursor. In practice, this usually means keeping the owning WiredTigerManagedSession
    // alive for at least as long as the cursor.
    WiredTigerPrepareCursor(WiredTigerSession& session);
    ~WiredTigerPrepareCursor();

    WT_CURSOR* get() const {
        return _cursor;
    }

    WT_CURSOR* operator->() const {
        return get();
    }

private:
    WT_CURSOR* _cursor = nullptr;  // Owned
    WiredTigerSession& _session;
};
}  // namespace mongo
