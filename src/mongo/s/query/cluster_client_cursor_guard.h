/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <memory>

#include "mongo/db/operation_context.h"
#include "mongo/s/query/cluster_client_cursor.h"

namespace mongo {

/**
 * An RAII object which owns a ClusterClientCursor and kills the cursor if it is not explicitly
 * released.
 */
class ClusterClientCursorGuard final {
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
        invariant(_ccc);
        return std::move(_ccc);
    }

private:
    OperationContext* _opCtx;
    std::unique_ptr<ClusterClientCursor> _ccc;
};

}  // namespace mongo
