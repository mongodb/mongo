// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/s/query/exec/cluster_client_cursor.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {

/**
 * An RAII object which owns a ClusterClientCursor and kills the cursor if it is not explicitly
 * released.
 */
class [[MONGO_MOD_PUBLIC]] ClusterClientCursorGuard final {
    ClusterClientCursorGuard(const ClusterClientCursorGuard&) = delete;
    ClusterClientCursorGuard& operator=(const ClusterClientCursorGuard&) = delete;

public:
    ClusterClientCursorGuard(OperationContext* opCtx, std::unique_ptr<ClusterClientCursor> ccc)
        : _opCtx(opCtx), _ccc(std::move(ccc)) {}

    /**
     * If a cursor is owned, safely destroys the cursor, cleaning up remote cursor state if
     * necessary. May block waiting for remote cursor cleanup.
     *
     * If no cursor is owned, does nothing.
     *
     * If the cursor has been killed by a previous command, does nothing. hasBeenKilled() will be
     * true if the cursor was killed while the cursor was checked out or in use with a Guard.
     */
    ~ClusterClientCursorGuard() {
        if (_ccc && !_ccc->remotesExhausted() && !_ccc->hasBeenKilled()) {
            _ccc->kill(_opCtx);
        }
    }

    ClusterClientCursorGuard(ClusterClientCursorGuard&&) = default;
    ClusterClientCursorGuard& operator=(ClusterClientCursorGuard&&) = default;

    /**
     * Returns a pointer to the underlying cursor.
     */
    ClusterClientCursor* operator->() {
        return _ccc.get();
    }

    /**
     * Returns a pointer to the underlying cursor.
     */
    ClusterClientCursor* get() {
        return _ccc.get();
    }

    /**
     * True if this ClusterClientCursorGuard owns a cursor.
     */
    explicit operator bool() const {
        return !!_ccc;
    }

    /**
     * Transfers ownership of the underlying cursor to the caller. The guard must own a cursor
     * when releaseCursor() is called; callers must first ensure the guard owns a cursor by
     * using operator bool or operator ->.
     */
    std::unique_ptr<ClusterClientCursor> releaseCursor() {
        tassert(11052320, "Expected ClusterClientCursorGuard to own a cursor", _ccc);
        return std::move(_ccc);
    }

private:
    OperationContext* _opCtx;
    std::unique_ptr<ClusterClientCursor> _ccc;
};

}  // namespace mongo
