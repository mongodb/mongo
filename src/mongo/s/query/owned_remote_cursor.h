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

#include "mongo/executor/task_executor_pool.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/async_results_merger_params_gen.h"
#include "mongo/s/query/establish_cursors.h"

namespace mongo {

/**
 * A RAII wrapper class for RemoteCursor which schedules a killCursors request upon destruction if
 * the cursor has not been released.
 */
class OwnedRemoteCursor {
public:
    OwnedRemoteCursor(const OwnedRemoteCursor&) = delete;
    OwnedRemoteCursor& operator=(const OwnedRemoteCursor&) = delete;

    OwnedRemoteCursor(OperationContext* opCtx, RemoteCursor&& cursor, NamespaceString nss)
        : _opCtx(opCtx), _remoteCursor(std::move(cursor)), _nss(std::move(nss)) {}

    ~OwnedRemoteCursor() {
        if (_remoteCursor) {
            killRemoteCursor(_opCtx,
                             Grid::get(_opCtx)->getExecutorPool()->getArbitraryExecutor(),
                             releaseCursor(),
                             _nss);
        }
    }

    OwnedRemoteCursor(OwnedRemoteCursor&& other)
        : _opCtx(other._opCtx), _remoteCursor(other.releaseCursor()), _nss(std::move(other._nss)) {}

    OwnedRemoteCursor& operator=(OwnedRemoteCursor&& other) {
        _remoteCursor = other.releaseCursor();
        _opCtx = other._opCtx;
        _nss = std::move(other._nss);
        return *this;
    }

    RemoteCursor* operator->() {
        invariant(_remoteCursor);
        return &(*_remoteCursor);
    }

    RemoteCursor* operator*() {
        invariant(_remoteCursor);
        return &(*_remoteCursor);
    }

    /**
     * Transfers ownership of the RemoteCursor to the caller, will not attempt to kill the cursor
     * when this object is destroyed.
     */
    RemoteCursor releaseCursor() {
        auto cursor = std::move(*_remoteCursor);
        _remoteCursor.reset();
        return cursor;
    }

private:
    OperationContext* _opCtx;
    boost::optional<RemoteCursor> _remoteCursor;
    NamespaceString _nss;
};

}  // namespace mongo
