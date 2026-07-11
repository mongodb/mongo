// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/sharding_environment/grid.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/s/query/exec/establish_cursors.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

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
            auto executor = Grid::get(_opCtx)->getExecutorPool()->getArbitraryExecutor();
            killRemoteCursor(_opCtx, executor.get(), releaseCursor(), _nss);
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
        tassert(11052342, "Expected OwnedRemoteCursor to own a cursor", _remoteCursor);
        return &(*_remoteCursor);
    }

    RemoteCursor* operator*() {
        tassert(11052343, "Expected OwnedRemoteCursor to own a cursor", _remoteCursor);
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
